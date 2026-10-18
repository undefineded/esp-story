#!/usr/bin/env node
/*
 * build_timeline.mjs —— 由音频产物估算一本故事的插画时间轴, 生成 timeline.lrc
 *
 * 用法:
 *   node tools/build_timeline.mjs 乘邮车来的十二位旅客
 *   node tools/build_timeline.mjs 乘邮车来的十二位旅客 --duration 743.59   # 覆盖总时长
 *   node tools/build_timeline.mjs 乘邮车来的十二位旅客 --dry-run           # 只打印, 不写文件
 *
 * 输入:
 *   s/<故事>/<故事>.音频分镜.json   音频的文本产物 (分句文本 + 停顿时长, 与音频段一一对应)
 *   s/<故事>/<故事>.插画时间轴.json  96 格内容 (sheets[] -> subPanels[] "NN-MM 摘要")
 *   mp3 实测时长 (ffprobe), 顺序: 音频/<故事>/*.mp3 -> 设备版/<故事>/*.mp3 -> JSON audioDurationSeconds
 *
 * 方法 (与《丑小鸭》 timingMethod = estimated_from_audio_storyboard_and_final_mp3 一致):
 *   1. 每段时长按 "正文字数 + 停顿折算字数" 的权重, 按比例分摊到 mp3 总时长
 *   2. 按段序拼出 "逐字 -> 时间" 索引 (停顿算在段尾, 不占用字的时间)
 *   3. 每张母图用它的 originalText 在旁白正文上做 3-gram 窗口匹配, 定出母图起止字符位
 *   4. 每格用它的摘要做 2-gram 匹配, 在母图区间内定位; 同一张的 4 格若不能严格递增则整张退化为等分
 *
 * 输出:
 *   s/<故事>/设备版/<故事>/timeline.lrc   [mm:ss.xx]NN-MM.jpg | 摘要   (设备播放用)
 *   s/<故事>/<故事>.子图时间轴.lrc        [mm:ss.xx]NN-MM.png | 摘要   (母图/切分源用)
 */
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/* ---------------- 参数 ---------------- */
const argv = process.argv.slice(2);
const story = argv.find((a) => !a.startsWith('--'));
const dryRun = argv.includes('--dry-run');
const di = argv.indexOf('--duration');
const durationOverride = di >= 0 ? Number(argv[di + 1]) : 0;

if (!story) {
  console.error('用法: node tools/build_timeline.mjs <故事名> [--duration 秒] [--dry-run]');
  process.exit(1);
}

const dir = path.join(ROOT, 's', story);
const readJson = (p) => JSON.parse(fs.readFileSync(p, 'utf8'));

/* ---------------- 工具函数 ---------------- */
/* 只保留汉字/字母/数字, 丢掉标点与空白 (标点在朗读里只是停顿, 已由 pauseAfterMs 表达) */
const norm = (s) => (s || '').replace(/[^\p{Script=Han}\p{L}\p{N}]/gu, '');

/* n 元组集合 */
function grams(s, n) {
  const out = new Set();
  for (let i = 0; i + n <= s.length; i++) out.add(s.slice(i, i + n));
  return out;
}

/* 时长 -> [mm:ss.xx] */
function ts(t) {
  const cs = Math.max(0, Math.round(t * 100));
  const mm = Math.floor(cs / 6000);
  const ss = Math.floor((cs % 6000) / 100);
  const ff = cs % 100;
  return `${String(mm).padStart(2, '0')}:${String(ss).padStart(2, '0')}.${String(ff).padStart(2, '0')}`;
}

/* 在 text 的 [from,to) 区间里滑动窗口, 返回命中 query 的 n-gram 最多、最靠前的起点 */
function locate(text, query, n, from, to, winFactor) {
  const qg = grams(query, n);
  if (!qg.size) return { pos: -1, score: 0 };
  const W = Math.max(Math.round(query.length * winFactor), query.length + n);
  let best = -1, bestScore = 0;
  const hi = Math.min(to, Math.max(from, text.length - 1));
  for (let p = from; p < hi; p++) {
    const w = text.substr(p, W);
    let score = 0;
    for (const g of qg) if (w.includes(g)) score++;
    if (score > bestScore) { bestScore = score; best = p; }
  }
  return { pos: best, score: bestScore, total: qg.size };
}

/* 最长公共子串: 返回 text 里与 query 完全一致的最长片段的 {长度, 起点} */
function longestCommonSubstring(text, query) {
  const m = query.length;
  if (!text.length || !m) return { len: 0, pos: -1 };
  let prev = new Uint16Array(m + 1);
  let cur = new Uint16Array(m + 1);
  let best = 0, bestPos = -1;
  for (let i = 1; i <= text.length; i++) {
    for (let j = 1; j <= m; j++) {
      cur[j] = text[i - 1] === query[j - 1] ? prev[j - 1] + 1 : 0;
      if (cur[j] > best) { best = cur[j]; bestPos = i - cur[j]; }
    }
    const t = prev; prev = cur; cur = t; cur.fill(0);
  }
  return { len: best, pos: bestPos };
}

/* mp3 实测时长 */
function mp3Duration() {
  const candidates = [];
  const pick = (d) => {
    if (!fs.existsSync(d)) return;
    for (const f of fs.readdirSync(d)) if (f.toLowerCase().endsWith('.mp3')) candidates.push(path.join(d, f));
  };
  pick(path.join(dir, '音频', story));
  pick(path.join(dir, '设备版', story));
  for (const f of candidates) {
    try {
      const out = execFileSync('ffprobe',
        ['-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', f],
        { encoding: 'utf8' }).trim();
      const d = Number(out);
      if (d > 0) return { seconds: d, file: path.relative(ROOT, f) };
    } catch { /* 该文件读不出来就试下一个 */ }
  }
  return null;
}

/* ---------------- 读输入 ---------------- */
/* 命名不统一: 有的素材带故事名前缀, 有的不带, 两个都试 */
const firstExisting = (names) => names.map((n) => path.join(dir, n)).find((p) => fs.existsSync(p));
const storyboardFile = firstExisting([`${story}.音频分镜.json`, '音频分镜.json']);
const timelineFile = firstExisting([`${story}.插画时间轴.json`, '插画时间轴.json']);
if (!storyboardFile) { console.error(`缺少输入: ${path.join(dir, '音频分镜.json')}`); process.exit(1); }
if (!timelineFile) { console.error(`缺少输入: ${path.join(dir, `${story}.插画时间轴.json`)}`); process.exit(1); }

const sb = readJson(storyboardFile);
const tl = readJson(timelineFile);

/* 96 格: 顺序拉平 "NN-MM 摘要" */
const panels = [];
for (const sheet of tl.sheets || []) {
  for (const sp of sheet.subPanels || []) {
    const m = sp.match(/^(\d{2}-\d{2})\s+(.*)$/);
    if (!m) { console.error(`subPanel 格式无法解析: ${sp}`); process.exit(1); }
    panels.push({ id: m[1], summary: m[2].trim() });
  }
}
if (!panels.length) { console.error('插画时间轴里没有 subPanels'); process.exit(1); }

/* 总时长 */
const measured = durationOverride > 0 ? { seconds: durationOverride, file: '命令行指定' } : mp3Duration();
const total = measured ? measured.seconds : Number(tl.timingPlan?.audioDurationSeconds || 0);
if (!(total > 0)) { console.error('拿不到音频总时长 (ffprobe 失败且 JSON 里也没有 audioDurationSeconds)'); process.exit(1); }

/* ---------------- 1. 分段时长: 朗读按实测速率, 停顿原样计入 ----------------
 * 不硬编码语速: 用 "总字数 / (总时长 - 停顿总时长)" 反推这本故事的实测朗读速率,
 * 于是 段时长 = 字数/速率 + 该段自己的停顿, 与音频总长严格自洽。 */
const segs = sb.segments.map((s) => ({
  id: s.id, speaker: s.speaker,
  chars: norm(s.text).length,
  pauseSec: Number(s.pauseAfterMs || 0) / 1000,
}));
const charsSum = segs.reduce((a, s) => a + s.chars, 0);
const pauseSum = segs.reduce((a, s) => a + s.pauseSec, 0);
const rate = charsSum / Math.max(1, total - pauseSum);   // 字/秒
const scale = 1 / rate;                                  // 每字秒数

let t = 0;
for (const s of segs) {
  s.start = t;
  s.speech = s.chars * scale;                              // 真正朗读的时长 (停顿不占字)
  s.duration = s.speech + s.pauseSec;                      // 含停顿
  t += s.duration;
}

/* ---------------- 2. 逐字 -> 时间 索引 ---------------- */
let full = '';
const posSeg = [];                               // 每个字符属于哪一段
const posOff = [];                               // 段内第几个字
for (let i = 0; i < segs.length; i++) {
  const txt = norm(sb.segments[i].text);
  for (let j = 0; j < txt.length; j++) { full += txt[j]; posSeg.push(i); posOff.push(j); }
}
const timeAt = (k) => {
  if (k <= 0) return 0;
  if (k >= full.length) return total;
  const i = posSeg[k], j = posOff[k];
  const s = segs[i];
  return s.start + (s.chars ? (j / s.chars) * s.speech : 0);
};

/* ---------------- 3. 母图边界: 最长公共子串定位 + 边界取中点 ----------------
 * 每张插画的 originalText 与音频旁白同源, 但被删改过, 词袋匹配噪声太大。
 * 改用"最长公共子串"作为该张在旁白里的证据位置, 精确得多。
 * 命中不了的母图 (只描述画面/服饰, 旁白里没有对应句子) 靠左右证据线性插值。
 * 边界取相邻两张证据位置的中点: 即使证据落在该张内容的中后段, 也不会把时长算长。 */
const MIN_EVIDENCE_RATIO = 0.2;
const sheets = (tl.sheets || []).map((sh, idx) => {
  const query = norm(sh.originalText);
  const c = longestCommonSubstring(full, query);
  const anchored = c.pos >= 0 && c.len >= Math.max(8, Math.round(query.length * MIN_EVIDENCE_RATIO));
  return {
    idx, id: sh.id, query, count: (sh.subPanels || []).length,
    startChar: anchored ? c.pos : -1, evLen: c.len, anchored,
  };
});

/* 证据位置必须递增, 回退的降级为插值 */
let prevChar = -1;
for (const sh of sheets) {
  if (!sh.anchored) continue;
  if (sh.startChar <= prevChar) sh.anchored = false;
  else prevChar = sh.startChar;
}

/* 代表时间: 有证据用证据位置换算, 没有的按左右最近的证据线性插值 */
const rep = new Array(sheets.length);
{
  let a = -1;
  for (let i = 0; i < sheets.length; i++) {
    if (!sheets[i].anchored) continue;
    const t1 = timeAt(sheets[i].startChar);
    if (a < 0) for (let k = 0; k < i; k++) rep[k] = (t1 * k) / i;      // 第一个证据之前: 从 0 起均分
    else { const t0 = rep[a]; for (let k = a + 1; k < i; k++) rep[k] = t0 + ((t1 - t0) * (k - a)) / (i - a); }
    rep[i] = t1; a = i;
  }
  if (a < 0) for (let i = 0; i < sheets.length; i++) rep[i] = (total * i) / Math.max(1, sheets.length - 1);
  else for (let k = a + 1; k < sheets.length; k++) rep[k] = rep[a] + ((total - rep[a]) * (k - a)) / (sheets.length - a);
}

/* 边界 = 相邻两张代表时间的中点; 首张从 0 起, 末张到片尾 */
for (let i = 0; i < sheets.length; i++) {
  sheets[i].spanA = i === 0 ? 0 : (rep[i - 1] + rep[i]) / 2;
  sheets[i].spanB = i === sheets.length - 1 ? total : (rep[i] + rep[i + 1]) / 2;
}

/* ---------------- 4. 格内定位: 摘要 2-gram 在母图区间内匹配, 否则等分 ---------------- */
const placed = [];
let pi = 0;
for (const sh of sheets) {
  const a = sh.spanA;
  const b = Math.max(a + 0.4, sh.spanB);
  const span = b - a;
  const cand = [];
  for (let k = 0; k < sh.count; k++) {
    const p = panels[pi + k];
    const near = sh.anchored
      ? locate(full, norm(p.summary), 2, sh.startChar, full.length, 2)
      : { pos: -1, score: 0, total: 0 };
    const need = Math.max(2, Math.ceil(near.total * 0.35));
    if (near.pos >= 0 && near.score >= need && timeAt(near.pos) < b)
      cand.push({ t: timeAt(near.pos), anchored: true });
    else cand.push({ t: a + (span * k) / sh.count, anchored: false });
  }
  const ok = cand.every((c, k) => c.t >= a && c.t < b && (k === 0 || c.t > cand[k - 1].t + 0.15));
  if (!ok) for (let k = 0; k < sh.count; k++) cand[k] = { t: a + (span * k) / sh.count, anchored: false };

  for (let k = 0; k < sh.count; k++) placed.push({ ...panels[pi + k], t: cand[k].t, anchored: cand[k].anchored, sheet: sh.id });
  pi += sh.count;
}

/* ---------------- 输出 ---------------- */
const toLrc = (ext) => placed.map((p) => `[${ts(p.t)}]${p.id}.${ext} | ${p.summary}`).join('\n') + '\n';

const targets = [
  { file: path.join(dir, '设备版', story, 'timeline.lrc'), ext: 'jpg' },
  { file: path.join(dir, `${story}.子图时间轴.lrc`), ext: 'png' },
];

console.log(`故事: ${story}`);
console.log(`段落: ${segs.length} 段 / 字数 ${segs.reduce((a, s) => a + s.chars, 0)} / 停顿合计 ${segs.reduce((a, s) => a + s.pauseSec, 0).toFixed(1)}s / 实测语速 ${rate.toFixed(2)} 字/秒`);
console.log(`总时长: ${total.toFixed(2)}s (${measured ? measured.file : 'JSON audioDurationSeconds'})`);
console.log(`格子: ${placed.length} (母图 ${sheets.length} 张)`);
console.log(`母图旁白证据: ${sheets.filter((s) => s.anchored).length}/${sheets.length} (其余靠插值)；格内摘要锚定: ${placed.filter((p) => p.anchored).length}/${placed.length}`);
console.log('');
for (const s of sheets) {
  const tag = s.anchored ? `证据 ${s.evLen} 字` : '插值 (该张只描述画面, 旁白无对应句)';
  console.log(`  母图 ${s.id}  ${ts(s.spanA)} → ${ts(s.spanB)}  ${(s.spanB - s.spanA).toFixed(1)}s (${((s.spanB - s.spanA) / s.count).toFixed(1)}s/格)  ${tag}`);
}
console.log('');
for (const t2 of targets) {
  const rel = path.relative(ROOT, t2.file);
  if (dryRun) { console.log(`[dry-run] 跳过写入 ${rel}`); continue; }
  fs.mkdirSync(path.dirname(t2.file), { recursive: true });
  fs.writeFileSync(t2.file, toLrc(t2.ext), 'utf8');
  console.log(`写出 ${rel}  (${placed.length} 行)`);
}
console.log('');
console.log('前 8 行预览:');
console.log(toLrc(targets[0].ext).split('\n').slice(0, 8).join('\n'));
