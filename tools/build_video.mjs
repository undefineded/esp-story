#!/usr/bin/env node
/**
 * 把普通视频 (mp4/mov/mkv/avi...) 转成故事盒能播的格式:
 *
 *   /video/<名字>.avi   320x240 固定帧率 (默认 12fps) 的帧内编码视频
 *   /video/<名字>.mp3   单声道 22.05kHz 音轨 (源视频没有音轨时不生成)
 *
 * 两种编码 (设备端都支持):
 *   --codec qoi  (默认)  QOI: 每像素只有查表/差分/RLE, 软解约 3~6ms/帧。
 *                        先量化到 RGB565 再编码, 与屏幕完全一致 (无损)。
 *                        体积约为 MJPEG 的 3~5 倍, 换来 3~5 倍解码速度。
 *   --codec jpeg         MJPEG: 体积小, 但 Huffman + IDCT + 上采样约 20~35ms/帧。
 *
 * 设备端 (ESP32-S3) 没有 H.264 硬解; MP4/H.264 不能直接播放, 必须先转换。
 *
 * 用法:
 *   node tools/build_video.mjs 动画.mp4
 *   node tools/build_video.mjs 第一集.mp4 第二集.mov --out D:\video --fps 15
 *   node tools/build_video.mjs --all-in ./素材 --out ./out --codec jpeg
 *
 * 参数:
 *   --out <dir>     输出目录 (默认 ./out-video)
 *   --codec <qoi|jpeg>  帧编码, 默认 qoi (软解最优)
 *   --fps <n>       帧率, 默认 12 (QOI 可到 15~20, JPEG 建议 <= 12)
 *   --quality <n>   仅 jpeg: 质量 2~31, 默认 6 (数字越小越清晰/越大)
 *   --width/--height  画面尺寸, 默认 320x240 (屏幕分辨率)
 *   --no-audio      不生成音轨
 *   --scale-crop    源比例不一致时裁切填满 (默认加黑边)
 */
import { execFileSync, spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, statSync, unlinkSync } from 'node:fs'
import { basename, extname, join, resolve } from 'node:path'
import { parseAvi } from './lib/avi.mjs'
import { createAviWriter } from './lib/avi-writer.mjs'
import { encodeQoi, quantize565 } from './lib/qoi.mjs'

const VIDEO_EXT = new Set(['.mp4', '.mov', '.mkv', '.avi', '.webm', '.m4v', '.wmv', '.flv', '.ts', '.mpg', '.mpeg'])
const MAX_FRAMES = 8000                 // 与固件 src/config.h 的 MAX_VIDEO_FRAMES 一致
const MAX_FRAME_BYTES = 192 * 1024      // 与固件 MAX_VIDEO_BYTES 一致

function parseArgs(argv) {
  const opts = {
    out: 'out-video', fps: 12, width: 320, height: 240, quality: 6,
    audio: true, crop: false, codec: 'qoi', inputs: [], allIn: null,
  }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--out') opts.out = argv[++i]
    else if (a === '--fps') opts.fps = Number(argv[++i])
    else if (a === '--width') opts.width = Number(argv[++i])
    else if (a === '--height') opts.height = Number(argv[++i])
    else if (a === '--quality') opts.quality = Number(argv[++i])
    else if (a === '--codec') opts.codec = String(argv[++i]).toLowerCase()
    else if (a === '--no-audio') opts.audio = false
    else if (a === '--scale-crop') opts.crop = true
    else if (a === '--all-in') opts.allIn = argv[++i]
    else if (a.startsWith('--')) throw new Error(`未知参数 ${a}`)
    else opts.inputs.push(a)
  }
  if (opts.allIn) {
    for (const name of readdirSync(opts.allIn)) {
      if (VIDEO_EXT.has(extname(name).toLowerCase())) opts.inputs.push(join(opts.allIn, name))
    }
  }
  if (!opts.inputs.length) throw new Error('请提供视频文件, 或 --all-in <目录>')
  if (opts.codec !== 'qoi' && opts.codec !== 'jpeg') throw new Error('--codec 只能是 qoi 或 jpeg')
  if (!Number.isFinite(opts.fps) || opts.fps <= 0 || opts.fps > 60) throw new Error('--fps 需在 1..60')
  return opts
}

const ffmpeg = (args) => execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args],
  { stdio: 'inherit' })

function hasAudioStream(path) {
  const out = execFileSync('ffprobe', ['-v', 'error', '-select_streams', 'a',
    '-show_entries', 'stream=index', '-of', 'csv=p=0', path]).toString().trim()
  return out.length > 0
}

function filterChain(opts) {
  const pad = opts.crop
    ? `scale=${opts.width}:${opts.height}:force_original_aspect_ratio=increase,crop=${opts.width}:${opts.height}`
    : `scale=${opts.width}:${opts.height}:force_original_aspect_ratio=decrease,` +
      `pad=${opts.width}:${opts.height}:(${opts.width}-iw)/2:(${opts.height}-ih)/2:color=black`
  return `fps=${opts.fps},${pad},setsar=1`
}

/* ---------------- QOI: ffmpeg 出 rawvideo, Node 编码, 自己封装 AVI ---------------- */

async function encodeQoiVideo(input, opts, aviPath) {
  const frameBytes = opts.width * opts.height * 3
  const child = spawn('ffmpeg', [
    '-hide_banner', '-loglevel', 'error', '-y',
    '-i', input, '-vf', filterChain(opts),
    '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1',
  ], { stdio: ['ignore', 'pipe', 'inherit'] })

  const writer = createAviWriter(aviPath, {
    width: opts.width, height: opts.height, fps: opts.fps, codec: 'QOI ',
  })

  let pending = Buffer.alloc(0)
  let frames = 0
  let maxFrame = 0
  try {
    for await (const chunk of child.stdout) {
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk
      while (pending.length >= frameBytes) {
        const frame = pending.subarray(0, frameBytes)
        pending = pending.subarray(frameBytes)
        const qoi = encodeQoi(quantize565(frame, opts.width, opts.height), opts.width, opts.height)
        writer.addFrame(qoi)
        frames++
        if (qoi.length > maxFrame) maxFrame = qoi.length
      }
    }
    const code = await new Promise((res) => child.on('close', res))
    if (code !== 0) throw new Error(`ffmpeg 退出码 ${code}`)
    if (!frames) throw new Error('ffmpeg 没有输出任何帧')
    writer.finish()
    return { frames, maxFrame }
  } catch (e) {
    try { writer.finish() } catch { /* ignore */ }
    try { unlinkSync(aviPath) } catch { /* ignore */ }   // 别留下只有文件头的半成品
    throw e
  }
}

/* ---------------- JPEG: 全交给 ffmpeg ---------------- */

function encodeJpegVideo(input, opts, aviPath) {
  ffmpeg([
    '-i', input,
    '-vf', filterChain(opts),
    '-c:v', 'mjpeg', '-q:v', String(opts.quality), '-pix_fmt', 'yuvj420p',
    '-an', '-f', 'avi', aviPath,
  ])
}

/* ---------------- 音轨 ---------------- */

function encodeAudio(input, opts, mp3Path) {
  const withAudio = opts.audio && hasAudioStream(input)
  if (withAudio) {
    ffmpeg(['-i', input, '-vn', '-ac', '1', '-ar', '22050',
      '-c:a', 'libmp3lame', '-b:a', '48k', mp3Path])
  } else if (existsSync(mp3Path)) {
    unlinkSync(mp3Path)              // 旧音轨会和新视频对不上
  }
  return withAudio
}

/* ---------------- 校验 ---------------- */

function report(name, aviPath, mp3Path, opts, withAudio) {
  const info = parseAvi(aviPath, { maxFrames: MAX_FRAMES + 1 })
  const worst = info.frames.reduce((m, f) => Math.max(m, f.size), 0)
  const problems = []
  const warnings = []

  if (info.width !== opts.width || info.height !== opts.height) {
    problems.push(`分辨率 ${info.width}x${info.height} (期望 ${opts.width}x${opts.height})`)
  }
  if (info.frames.length > MAX_FRAMES) problems.push(`帧数 ${info.frames.length} 超过固件上限 ${MAX_FRAMES}`)
  if (worst > MAX_FRAME_BYTES) problems.push(`单帧 ${(worst / 1024).toFixed(0)}KB 超过 ${MAX_FRAME_BYTES / 1024}KB`)
  if (!withAudio) warnings.push('没有音轨 (设备上静音播放)')

  const mb = (statSync(aviPath).size / 1024 / 1024).toFixed(1)
  const avgKb = (info.frames.reduce((s, f) => s + f.size, 0) / info.frames.length / 1024).toFixed(1)
  console.log(`\n${name}`)
  console.log(`  ${aviPath}  ${info.codec.trim()} ${info.width}x${info.height} ${info.fps}fps ` +
    `${info.frames.length}帧 ${(info.durationMs / 1000).toFixed(1)}s ${mb}MB`)
  console.log(`  ${withAudio ? mp3Path + '  (单声道 22.05kHz)' : '无音轨'}  ·  平均 ${avgKb}KB/帧`)
  for (const w of warnings) console.log(`  · ${w}`)
  for (const p of problems) console.log(`  ! ${p}`)
  if (!problems.length) console.log('  OK: 可以直接拷进 TF 卡的 /video 目录')
  return { ok: problems.length === 0 }
}

async function convert(input, opts) {
  const name = basename(input, extname(input))
  const aviPath = join(opts.out, `${name}.avi`)
  const mp3Path = join(opts.out, `${name}.mp3`)

  if (opts.codec === 'qoi') await encodeQoiVideo(input, opts, aviPath)
  else                      encodeJpegVideo(input, opts, aviPath)

  const withAudio = encodeAudio(input, opts, mp3Path)
  return report(name, aviPath, mp3Path, opts, withAudio)
}

async function main() {
  const opts = parseArgs(process.argv.slice(2))
  mkdirSync(opts.out, { recursive: true })
  console.log(`输出目录: ${resolve(opts.out)}  ·  编码 ${opts.codec === 'qoi' ? 'QOI (软解最优)' : 'MJPEG'}`)
  let ok = 0
  for (const input of opts.inputs) {
    if (!existsSync(input)) { console.log(`跳过 (找不到): ${input}`); continue }
    if ((await convert(input, opts)).ok) ok++
  }
  console.log(`\n完成: ${ok}/${opts.inputs.length} 个文件通过检查`)
  if (ok !== opts.inputs.length) process.exitCode = 1
}

main().catch((e) => {
  console.error(`失败: ${e.message}`)
  process.exitCode = 1
})
