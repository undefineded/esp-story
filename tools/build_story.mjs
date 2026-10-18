#!/usr/bin/env node
/**
 * 把故事素材整理成故事机(SD 卡)可直接使用的「设备版」目录。
 *
 * 时间轴来源 (自动判定, 二选一):
 *   1) <故事>.插画时间轴.json —— 其中 items[].image 全部有效(存在且 >1KB)时使用
 *   2) <故事>.子图时间轴.lrc  —— 形如 [mm:ss.xx]NN-MM.png | 摘要;
 *      图片按文件名在故事文件夹内递归查找, 自动挑「有效图片最多」的 lrc
 *      (源文件夹里的 插画/NN.png、四宫格插画/NN.png 可能只是空壳占位)
 *
 * 音频来源: json.audio 指向的文件有效则用它; 否则取故事文件夹内体积最大的 *.mp3
 *
 * 输出: <故事文件夹>/设备版/<故事名>/
 *         NN.jpg / NN-MM.jpg   320x240 JPEG (画面完整贴顶, 下方黑边留给进度条)
 *         <故事名>.mp3          单声道 64kbps MP3
 *         timeline.lrc          [mm:ss.xx]<图名>.jpg | 摘要
 *
 * 该目录整体拷到 SD 卡的 /story/ 下即为 <故事名> 故事。
 * 固件在下方黑边区独立渲染进度条, 不覆盖插画。
 *
 * 用法: node tools/build_story.mjs "<故事文件夹>"
 * 依赖: ffmpeg (可用 FFMPEG 环境变量指定路径)
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { basename, join, relative, resolve } from 'node:path';

const FFMPEG = process.env.FFMPEG || 'ffmpeg';

const SCR_W = 320;          // 屏幕宽
const SCR_H = 240;          // 屏幕高
const IMG_H = 214;          // 插画内容区高度: 下方 240-214=26px 黑边留给进度条
const MP3_BITRATE = '64k';  // 单声道码率
const MP3_RATE = 24000;     // 与源音频采样率一致
const JPG_Q = '4';          // JPEG 质量 (数字越小越好, 2..5 合适)
const MIN_VALID = 1024;     // 小于此字节数的文件视为无效空壳

const srcArg = process.argv[2];
if (!srcArg) {
  console.error('用法: node tools/build_story.mjs "<故事文件夹>"');
  process.exit(2);
}
const srcDir = resolve(srcArg);
if (!existsSync(srcDir) || !statSync(srcDir).isDirectory()) {
  console.error(`[ERR] 找不到故事文件夹: ${srcDir}`);
  process.exit(2);
}

const OUT_ROOT = '设备版';   // 产物目录名 (位于故事文件夹内, 属新增文件)

/* 递归列出文件; 跳过产物目录, 避免把上一次的输出当成素材 */
function walk(dir, acc = []) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) { if (e.name !== OUT_ROOT) walk(p, acc); }
    else acc.push(p);
  }
  return acc;
}
const allFiles = walk(srcDir);
const sizeOf = (p) => (p && existsSync(p) && statSync(p).isFile() ? statSync(p).size : 0);
const byBase = new Map();     // 文件名 -> 绝对路径 (递归查找用)
for (const p of allFiles) if (!byBase.has(basename(p))) byBase.set(basename(p), p);
const largestByExt = (ext) => {
  let best = null, bestSize = MIN_VALID;
  for (const p of allFiles) {
    if (p.toLowerCase().endsWith(ext) && sizeOf(p) > bestSize) { best = p; bestSize = sizeOf(p); }
  }
  return best;
};

/* ---------- 时间轴: json 有效则优先, 否则退化为 lrc ---------- */
let title = basename(srcDir);
let jsonAudio = null;
let items = null;

const tlName = readdirSync(srcDir).find((n) => n.endsWith('.插画时间轴.json'));
if (tlName) {
  let tl = null;
  try {
    tl = JSON.parse(readFileSync(join(srcDir, tlName), 'utf8').replace(/^\uFEFF/, ''));
  } catch {
    console.log(`[WARN] ${tlName} 不是有效 JSON (可能是 Git LFS 指针), 改用 lrc 时间轴`);
  }
  if (tl) {
    title = tl.title || title;
    jsonAudio = tl.audio ? join(srcDir, tl.audio) : null;
    const list = tl.items || [];
    const ok = list.length > 0 && list.every((it) => sizeOf(join(srcDir, it.image || '')) > MIN_VALID);
    if (ok) {
      items = list.map((it) => ({ name: it.panel, image: join(srcDir, it.image), start: it.start, summary: it.summary || '' }));
      console.log(`[TL ] json 时间轴 ${tlName} (${items.length} 幅)`);
    } else {
      console.log(`[WARN] ${tlName} 引用的插画无效(空壳), 改用 lrc 时间轴`);
    }
  }
}

if (!items) {
  let best = null;
  for (const n of readdirSync(srcDir).filter((f) => f.toLowerCase().endsWith('.lrc'))) {
    const list = [];
    for (const l of readFileSync(join(srcDir, n), 'utf8').split(/\r?\n/)) {
      const m = l.match(/^\[(\d{1,2}:\d{2}\.\d{2})\](.+?)\s*\|\s*(.*)$/);
      if (!m) continue;
      const file = byBase.get(m[2].trim());
      if (!file || sizeOf(file) <= MIN_VALID) continue;      // 跳过空壳图片
      list.push({ name: m[2].trim().replace(/\.[^.]+$/, ''), image: file, start: m[1], summary: m[3].trim() });
    }
    if (list.length && (!best || list.length > best.items.length)) best = { name: n, items: list };
  }
  if (best) {
    items = best.items;
    console.log(`[TL ] lrc 时间轴 ${best.name} (${items.length} 幅)`);
  }
}

if (!items || items.length === 0) {
  console.error('[ERR] 找不到可用的时间轴 (既无有效 json, 也无有效 lrc)');
  if (allFiles.some((p) => sizeOf(p) <= MIN_VALID)) {
    console.error('      提示: 存在 1KB 以下的文件, 疑似 Git LFS 指针 (真实内容尚未拉取)');
  }
  process.exit(2);
}

/* ---------- 输出目录 (新文件夹) ---------- */
const outDir = join(srcDir, OUT_ROOT, title);
mkdirSync(outDir, { recursive: true });
console.log(`[OUT] ${outDir}`);

const run = (args) =>
  execFileSync(FFMPEG, ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'inherit' });

/* ---- 插画: 任意比例 -> 320x240, 画面完整贴顶, 黑边全部留在下方 ---- */
const VF =
  `scale=${SCR_W}:${IMG_H}:force_original_aspect_ratio=decrease:force_divisible_by=2,` +
  `pad=${SCR_W}:${SCR_H}:(ow-iw)/2:0:black`;

const lrcLines = [];
for (const it of items) {
  const to = join(outDir, `${it.name}.jpg`);
  run(['-i', it.image, '-vf', VF, '-frames:v', '1', '-q:v', JPG_Q, '-pix_fmt', 'yuvj420p', to]);
  lrcLines.push(`[${it.start}]${it.name}.jpg | ${it.summary}`);
  console.log(`       ${it.name}.jpg  <- ${relative(srcDir, it.image)}`);
}

/* ---- 音频: 优先用 json 指定的文件; 无效则取体积最大的 mp3 ---- */
const audioSrc = sizeOf(jsonAudio) > MIN_VALID ? jsonAudio : largestByExt('.mp3');
if (!audioSrc) {
  console.error('[ERR] 找不到有效音频 (wav 无效且无可用 mp3)');
  process.exit(2);
}
const mp3 = join(outDir, `${title}.mp3`);
run(['-i', audioSrc, '-vn', '-c:a', 'libmp3lame', '-b:a', MP3_BITRATE, '-ac', '1', '-ar', String(MP3_RATE), mp3]);
console.log(`       ${title}.mp3  <- ${relative(srcDir, audioSrc)}`);

/* ---- 时间轴: 供固件按播放进度切换插画 ---- */
writeFileSync(join(outDir, 'timeline.lrc'), lrcLines.join('\n') + '\n', 'utf8');
console.log(`       timeline.lrc  (${items.length} 幅)`);

/* ---- 结果汇总 ---- */
const kb = (p) => (statSync(p).size / 1024).toFixed(1).padStart(8);
let total = 0;
for (const n of readdirSync(outDir).sort()) {
  const p = join(outDir, n);
  total += statSync(p).size;
  console.log(`       ${kb(p)} KB  ${n}`);
}
console.log(`[OK ] 合计 ${(total / 1024 / 1024).toFixed(2)} MB -> 拷到 SD 卡 /story/${title}/`);
