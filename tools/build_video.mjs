#!/usr/bin/env node
/**
 * ESP-IDF esp_player assets: 320x240 MJPEG AVI with an embedded MP3 track.
 * Usage: node tools/build_video.mjs input.mp4 --out ./out-video --fps 15
 * --no-audio writes silence (keeps a stable audio clock). QOI is no longer supported.
 * Existing QOI clips must be regenerated from their original source.
 */
import { execFileSync } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, renameSync, statSync, unlinkSync } from 'node:fs'
import { basename, extname, join, resolve } from 'node:path'

const VIDEO_EXT = new Set(['.mp4', '.mov', '.mkv', '.avi', '.webm', '.m4v', '.ts', '.mpg', '.mpeg'])
function parseArgs(argv) {
  const opts = { out: 'out-video', fps: 15, quality: 6, audio: true, crop: false, inputs: [] }
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i]
    const value = () => { if (!argv[i + 1]) throw new Error(a + ' 缺少参数'); return argv[++i] }
    if (a === '--out') opts.out = value()
    else if (a === '--fps') opts.fps = Number(value())
    else if (a === '--quality') opts.quality = Number(value())
    else if (a === '--width') { if (Number(value()) !== 320) throw new Error('仅支持 320x240') }
    else if (a === '--height') { if (Number(value()) !== 240) throw new Error('仅支持 320x240') }
    else if (a === '--codec') { if (value() !== 'jpeg') throw new Error('官方播放器使用 --codec jpeg；不再支持 QOI') }
    else if (a === '--no-audio') opts.audio = false
    else if (a === '--scale-crop') opts.crop = true
    else if (a === '--all-in') {
      const dir = value()
      opts.inputs.push(...readdirSync(dir).filter(n => VIDEO_EXT.has(extname(n).toLowerCase())).map(n => join(dir, n)))
    } else if (a.startsWith('--')) throw new Error('未知参数 ' + a)
    else opts.inputs.push(a)
  }
  if (!opts.inputs.length) throw new Error('请提供视频文件或 --all-in <目录>')
  if (!Number.isInteger(opts.fps) || opts.fps < 1 || opts.fps > 30) throw new Error('--fps 需在 1..30')
  if (!Number.isInteger(opts.quality) || opts.quality < 2 || opts.quality > 31) throw new Error('--quality 需在 2..31')
  return opts
}
function probe(path) {
  return JSON.parse(execFileSync('ffprobe', ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', path], { encoding: 'utf8' }))
}
function convert(input, opts) {
  const destination = join(opts.out, basename(input, extname(input)) + '.avi')
  if (resolve(input) === resolve(destination)) throw new Error('输入和输出不能是同一文件')
  const temp = destination + '.partial-' + process.pid
  const hasAudio = opts.audio && probe(input).streams.some(s => s.codec_type === 'audio')
  const scale = opts.crop
    ? 'scale=320:240:force_original_aspect_ratio=increase,crop=320:240'
    : 'scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(320-iw)/2:(240-ih)/2:color=black'
  const args = ['-hide_banner', '-loglevel', 'error', '-y', '-i', input]
  if (!hasAudio) args.push('-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo')
  args.push('-map', '0:v:0', '-map', hasAudio ? '0:a:0' : '1:a:0',
    '-vf', 'fps=' + opts.fps + ',' + scale + ',setsar=1',
    '-c:v', 'mjpeg', '-q:v', String(opts.quality), '-pix_fmt', 'yuvj420p',
    '-c:a', 'libmp3lame', '-ar', '44100', '-ac', '2', '-b:a', '96k',
    '-af', 'apad', '-shortest', '-f', 'avi', temp)
  try {
    execFileSync('ffmpeg', args, { stdio: 'inherit' })
    const info = probe(temp)
    const video = info.streams.find(s => s.codec_type === 'video')
    const audio = info.streams.find(s => s.codec_type === 'audio')
    if (!video || video.codec_name !== 'mjpeg' || video.width !== 320 || video.height !== 240 ||
        !audio || audio.codec_name !== 'mp3') throw new Error('输出音视频格式校验失败')
    renameSync(temp, destination)
    const seconds = Number(info.format.duration)
    const kbps = seconds > 0 ? statSync(destination).size / 1024 / seconds : 0
    console.log(destination + '  MJPEG 320x240 ' + opts.fps + 'fps + 内嵌 MP3' +
                (hasAudio ? '' : '（静音）') + (kbps ? '  ' + kbps.toFixed(0) + ' KB/s' : ''))
    // The device reads video and its embedded audio from the same 1-bit TF card
    // (~150-200 KB/s measured); above ~100 KB/s the audio pipeline starts to starve.
    if (kbps > 100) console.warn('  ! 码率偏高：建议 ≤100 KB/s（--fps 10 --quality 12 约 75 KB/s）')
  } finally {
    if (existsSync(temp)) unlinkSync(temp)
  }
}
try {
  const opts = parseArgs(process.argv.slice(2))
  mkdirSync(opts.out, { recursive: true })
  const targets = opts.inputs.map(p => basename(p, extname(p)) + '.avi')
  if (new Set(targets).size !== targets.length) throw new Error('多个输入文件具有相同输出名称')
  for (const input of opts.inputs) convert(input, opts)
} catch (e) {
  console.error('失败: ' + e.message)
  process.exitCode = 1
}
