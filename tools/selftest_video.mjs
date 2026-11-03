#!/usr/bin/env node
/**
 * 转码链路自测 (不需要硬件, 不需要 ffmpeg):
 *   1. QOI 编解码往返 (渐变 + 随机噪声, 覆盖 INDEX/DIFF/LUMA/RUN/RGB 各分支)
 *   2. AVI 封装器 -> 解析器往返 (codec/帧数/帧率/尺寸/每帧内容)
 *
 * 用法: node tools/selftest_video.mjs
 */
import { mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { decodeQoi, encodeQoi, quantize565 } from './lib/qoi.mjs'
import { createAviWriter } from './lib/avi-writer.mjs'
import { parseAvi } from './lib/avi.mjs'

let failures = 0
const check = (ok, message) => {
  if (!ok) { failures++; console.error(`✗ ${message}`) }
  else console.log(`✓ ${message}`)
}

function makeGradient(w, h) {
  const rgb = Buffer.alloc(w * h * 3)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 3
      rgb[i] = (x * 255 / (w - 1)) | 0
      rgb[i + 1] = (y * 255 / (h - 1)) | 0
      rgb[i + 2] = ((x + y) * 255 / (w + h - 2)) | 0
    }
  }
  return quantize565(rgb, w, h)
}

function makeNoise(w, h, seed) {
  const rgb = Buffer.alloc(w * h * 3)
  let s = seed >>> 0
  for (let i = 0; i < w * h * 3; i++) {
    s = (s * 1664525 + 1013904223) >>> 0
    rgb[i] = s >> 24
  }
  return rgb
}

function qoiRoundTrip(name, rgb, w, h) {
  const qoi = encodeQoi(rgb, w, h)
  const back = decodeQoi(qoi)
  check(back.width === w && back.height === h, `${name}: 尺寸`)
  check(Buffer.compare(back.rgb, rgb) === 0, `${name}: 像素无损 (${(qoi.length / 1024).toFixed(1)}KB)`)
  return qoi
}

function aviRoundTrip() {
  const w = 64, h = 48, fps = 12
  const frames = [
    makeGradient(w, h),
    makeNoise(w, h, 7),
    makeGradient(w, h).reverse(),
  ]
  const encoded = frames.map((f) => encodeQoi(f, w, h))
  const dir = mkdtempSync(join(tmpdir(), 'storybox-selftest-'))
  const path = join(dir, 'test.avi')
  try {
    const writer = createAviWriter(path, { width: w, height: h, fps, codec: 'QOI ' })
    for (const e of encoded) writer.addFrame(e)
    const stats = writer.finish()

    const info = parseAvi(path)
    check(info.codec.toUpperCase() === 'QOI ', 'AVI: codec 回读为 QOI')
    check(info.width === w && info.height === h, 'AVI: 尺寸回读')
    check(info.fps === fps, `AVI: 帧率回读 (${info.fps})`)
    check(info.frames.length === frames.length && stats.frames === frames.length, 'AVI: 帧数回读')
    check(info.indexed === true, 'AVI: 内嵌帧索引 (SBXI)')
    check(info.durationMs === Math.round(frames.length * 1e6 / fps / 1000), 'AVI: 时长回读')

    const data = readFileSync(path)
    let allMatch = true
    info.frames.forEach((f, i) => {
      const qoi = data.subarray(f.offset, f.offset + f.size)
      const back = decodeQoi(qoi)
      if (Buffer.compare(back.rgb, frames[i]) !== 0) allMatch = false
    })
    check(allMatch, 'AVI: 每帧解码后与源像素一致')

    /* 索引项必须和 movi 里真实的块位置一致 (固件读索引后会 +8 跳到数据区) */
    const at = data.indexOf('SBXI')
    let indexOk = at > 0
    if (indexOk) {
      const count = data.readUInt32LE(at + 12)
      indexOk = count === frames.length && data.readUInt32LE(at + 4) === 8 + count * 8
      for (let k = 0; indexOk && k < count; k++) {
        const off = data.readUInt32LE(at + 16 + k * 8)
        const len = data.readUInt32LE(at + 20 + k * 8)
        indexOk = off + 8 === info.frames[k].offset && len === info.frames[k].size
      }
    }
    check(indexOk, 'AVI: 索引指向真实的帧块')
  } finally {
    rmSync(dir, { recursive: true, force: true })
  }
}

qoiRoundTrip('渐变 320x240', makeGradient(320, 240), 320, 240)
qoiRoundTrip('噪声 320x240', makeNoise(320, 240, 1), 320, 240)
aviRoundTrip()

if (failures) {
  console.error(`\n${failures} 项失败`)
  process.exitCode = 1
} else {
  console.log('\n全部通过')
}
