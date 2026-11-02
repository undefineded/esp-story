#!/usr/bin/env node
/**
 * 检查 /video 里的文件能不能被故事盒播放 (不用上机)。
 *
 *   node tools/verify_video.mjs D:\video
 *   node tools/verify_video.mjs 动画.avi
 *
 * 检查项与固件 src/video.cpp 一致:
 *   - RIFF/AVI 容器、movi 列表、strf 编码为 MJPG 或 QOI
 *   - 帧数 <= 8000, 单帧 <= 192KB (PSRAM 帧缓冲上限)
 *   - 建议 320x240 (其它尺寸也能播, 但会超出屏幕/被裁切)
 *   - 同名 .mp3 / .wav 音轨是否存在
 */
import { existsSync, readdirSync, statSync } from 'node:fs'
import { extname, join } from 'node:path'
import { parseAvi } from './lib/avi.mjs'

const MAX_FRAMES = 8000
const MAX_FRAME_BYTES = 192 * 1024
const CODECS = new Set(['MJPG', 'QOI '])
const VIDEO_EXT = new Set(['.avi', '.mjpg', '.mjpeg'])

function collect(target) {
  if (!existsSync(target)) return []
  if (statSync(target).isDirectory()) {
    return readdirSync(target)
      .filter((n) => VIDEO_EXT.has(extname(n).toLowerCase()))
      .sort()
      .map((n) => join(target, n))
  }
  return [target]
}

function check(file) {
  const issues = []      // 会让设备播不了
  const warnings = []    // 能播, 但值得知道
  let info
  try {
    info = parseAvi(file)
  } catch (e) {
    console.log(`✗ ${file}\n    ${e.message}`)
    return false
  }

  const worst = info.frames.reduce((m, f) => Math.max(m, f.size), 0)
  if (!CODECS.has(info.codec.toUpperCase())) issues.push(`编码 ${info.codec.trim()} 不支持 (需要 MJPG 或 QOI)`)
  if (info.frames.length > MAX_FRAMES) issues.push(`帧数 ${info.frames.length} > ${MAX_FRAMES}`)
  if (worst > MAX_FRAME_BYTES) issues.push(`最大帧 ${(worst / 1024) | 0}KB > ${MAX_FRAME_BYTES / 1024}KB`)
  if (info.width !== 320 || info.height !== 240) warnings.push(`分辨率 ${info.width}x${info.height} ≠ 320x240`)

  const base = file.replace(/\.[^.]+$/, '')
  const audio = ['.mp3', '.wav'].find((e) => existsSync(base + e))
  if (!audio) warnings.push('没有同名音轨 (会静音播放)')

  const mb = (info.size / 1024 / 1024).toFixed(1)
  console.log(`${issues.length ? '✗' : '✓'} ${file}`)
  console.log(`    ${info.codec.trim()} ${info.width}x${info.height} ${info.fps}fps ${info.frames.length}帧 ` +
    `${(info.durationMs / 1000).toFixed(1)}s ${mb}MB · ` +
    `音轨 ${audio ? base + audio : '无'}`)
  if (info.indexed) console.log('    · 内嵌帧索引 (设备打开时不用逐帧扫卡)')
  for (const w of warnings) console.log(`    · ${w}`)
  for (const i of issues) console.log(`    ! ${i}`)
  return issues.length === 0
}

const targets = process.argv.slice(2)
if (!targets.length) {
  console.log('用法: node tools/verify_video.mjs <文件或目录> [...]')
  process.exit(2)
}

let total = 0
let bad = 0
for (const t of targets) {
  for (const file of collect(t)) {
    total++
    if (!check(file)) bad++
  }
}
console.log(`\n共 ${total} 个视频, ${bad} 个需要处理`)
if (bad) process.exitCode = 1
