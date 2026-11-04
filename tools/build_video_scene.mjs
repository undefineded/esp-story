#!/usr/bin/env node
/**
 * 历史版本：此脚本会覆盖现用插画。现用资源请运行 build_video_theater.py。
 * 画主页"视频剧场"插画 (128x128), 输出:
 *   docs/icons/video-scene.png   627x627 原图 (方便以后改配色/构图)
 *   src/home_video_scene.h       128x128 swap565 数组, 放 Flash, 运行时不解码 PNG
 *
 * 为什么不用 Pillow: 仓库只需要 Node + ffmpeg (tools/build_story.mjs 已经这么用)。
 * 这里用 Node 逐像素画, 交给 ffmpeg 编码 PNG; 缩到 128 用面积平均, 天然抗锯齿。
 *
 * 另外三格 (音乐/故事/设置) 仍用 docs/icons/home-scenes.png 的原数据
 * (src/home_scenes.h), 本脚本不碰它们。
 *
 * 用法: node tools/build_video_scene.mjs
 */
import { execFileSync } from 'node:child_process'
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..')
const PNG = join(ROOT, 'docs', 'icons', 'video-scene.png')
const HEADER = join(ROOT, 'src', 'home_video_scene.h')
const SIZE = 627
const OUT = 128
const BACKGROUND = '#f8dfd4'

/* ---------------- 极简绘图 ---------------- */

const rgb = (hex) => {
  const n = parseInt(hex.slice(1), 16)
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255]
}

function makeCanvas(w, h, fill) {
  const buf = Buffer.alloc(w * h * 4)
  for (let i = 0; i < w * h; i++) {
    buf[i * 4] = fill[0]
    buf[i * 4 + 1] = fill[1]
    buf[i * 4 + 2] = fill[2]
    buf[i * 4 + 3] = 255
  }
  return { w, h, buf }
}

function paint(cv, x0, y0, x1, y1, color, inside) {
  const bx0 = Math.max(0, Math.floor(x0))
  const by0 = Math.max(0, Math.floor(y0))
  const bx1 = Math.min(cv.w, Math.ceil(x1))
  const by1 = Math.min(cv.h, Math.ceil(y1))
  for (let y = by0; y < by1; y++) {
    for (let x = bx0; x < bx1; x++) {
      if (!inside(x + 0.5, y + 0.5)) continue
      const i = (y * cv.w + x) * 4
      cv.buf[i] = color[0]
      cv.buf[i + 1] = color[1]
      cv.buf[i + 2] = color[2]
      cv.buf[i + 3] = 255
    }
  }
}

const rect = (cv, x, y, w, h, c) => paint(cv, x, y, x + w, y + h, c, () => true)

function roundRect(cv, x, y, w, h, r, color) {
  r = Math.min(r, w / 2, h / 2)
  paint(cv, x, y, x + w, y + h, color, (px, py) => {
    const dx = Math.max(x + r - px, 0, px - (x + w - r))
    const dy = Math.max(y + r - py, 0, py - (y + h - r))
    return dx * dx + dy * dy <= r * r
  })
}

function circle(cv, cx, cy, r, color) {
  paint(cv, cx - r, cy - r, cx + r, cy + r, color,
    (px, py) => (px - cx) ** 2 + (py - cy) ** 2 <= r * r)
}

function ellipse(cv, cx, cy, rx, ry, color) {
  paint(cv, cx - rx, cy - ry, cx + rx, cy + ry, color,
    (px, py) => ((px - cx) / rx) ** 2 + ((py - cy) / ry) ** 2 <= 1)
}

function triangle(cv, a, b, c, color) {
  const [ax, ay] = a, [bx, by] = b, [cx, cy] = c
  const area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
  paint(cv, Math.min(ax, bx, cx), Math.min(ay, by, cy),
    Math.max(ax, bx, cx), Math.max(ay, by, cy), color, (px, py) => {
      const w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area
      const w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area
      const w2 = 1 - w0 - w1
      return w0 >= 0 && w1 >= 0 && w2 >= 0
    })
}

function thickLine(cv, x0, y0, x1, y1, t, color) {
  const dx = x1 - x0, dy = y1 - y0
  const len2 = dx * dx + dy * dy
  paint(cv, Math.min(x0, x1) - t, Math.min(y0, y1) - t,
    Math.max(x0, x1) + t, Math.max(y0, y1) + t, color, (px, py) => {
      let u = len2 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0
      u = Math.max(0, Math.min(1, u))
      const ex = x0 + u * dx - px, ey = y0 + u * dy - py
      return ex * ex + ey * ey <= (t / 2) ** 2
    })
}

/* 四角星 (超椭圆的星形) */
function sparkle(cv, cx, cy, r, color) {
  paint(cv, cx - r, cy - r, cx + r, cy + r, color, (px, py) => {
    const dx = Math.abs(px - cx), dy = Math.abs(py - cy)
    return (dx / r) ** 0.75 + (dy / r) ** 0.75 <= 1
  })
}

/* ---------------- 插画: 视频剧场 ---------------- */

function drawVideoScene() {
  const cv = makeCanvas(SIZE, SIZE, rgb(BACKGROUND))
  const INK = rgb('#2e4a4a')
  const TEAL = rgb('#58bfb0')
  const TEAL_LT = rgb('#8fd8cd')
  const SCREEN = rgb('#26304a')
  const SCREEN_HL = rgb('#2f3d63')
  const YELLOW = rgb('#ffc83d')
  const CREAM = rgb('#ffe79a')
  const PINK = rgb('#e9a9a0')
  const GREEN = rgb('#a8d7b9')

  /* 背景光晕 + 地毯 */
  circle(cv, 313, 320, 272, rgb('#fdefe8'))
  ellipse(cv, 330, 556, 246, 46, PINK)
  ellipse(cv, 330, 556, 210, 32, rgb('#f2c1b8'))

  /* 天线 (画在机身后面) */
  thickLine(cv, 300, 168, 214, 62, 12, INK)
  thickLine(cv, 334, 168, 432, 78, 12, INK)
  circle(cv, 214, 62, 20, INK)
  circle(cv, 214, 62, 14, YELLOW)
  circle(cv, 432, 78, 18, INK)
  circle(cv, 432, 78, 12, GREEN)

  /* 电视机: 墨色描边 + 青色机身 */
  roundRect(cv, 100, 148, 434, 334, 48, INK)
  roundRect(cv, 110, 158, 414, 314, 42, TEAL)
  roundRect(cv, 132, 178, 118, 24, 12, TEAL_LT)

  /* 屏幕: 深蓝底 + 播放三角 */
  roundRect(cv, 148, 206, 246, 208, 24, INK)
  roundRect(cv, 160, 218, 222, 184, 18, SCREEN)
  roundRect(cv, 160, 218, 222, 52, 18, SCREEN_HL)
  triangle(cv, [252, 266], [252, 356], [320, 311], INK)
  triangle(cv, [263, 280], [263, 342], [311, 311], YELLOW)
  circle(cv, 352, 238, 12, CREAM)
  sparkle(cv, 196, 246, 14, CREAM)

  /* 右侧旋钮与喇叭 */
  circle(cv, 448, 262, 32, INK)
  circle(cv, 448, 262, 24, CREAM)
  thickLine(cv, 437, 262, 459, 262, 6, INK)
  circle(cv, 448, 344, 24, INK)
  circle(cv, 448, 344, 17, PINK)
  for (let i = 0; i < 3; i++) roundRect(cv, 406, 396 + i * 22, 92, 10, 5, rgb('#3f8c84'))

  /* 机脚 */
  rect(cv, 214, 470, 24, 40, INK)
  rect(cv, 396, 470, 24, 40, INK)
  roundRect(cv, 196, 502, 60, 16, 8, INK)
  roundRect(cv, 378, 502, 60, 16, 8, INK)

  /* 角落点缀 */
  sparkle(cv, 512, 138, 34, YELLOW)
  sparkle(cv, 92, 168, 22, GREEN)
  sparkle(cv, 150, 402, 16, CREAM)
  return cv
}

/* ---------------- 缩放 + 头文件 ---------------- */

function boxDownsample(cv, outSize) {
  const out = Buffer.alloc(outSize * outSize * 4)
  const step = cv.w / outSize
  for (let y = 0; y < outSize; y++) {
    const y0 = y * step, y1 = (y + 1) * step
    for (let x = 0; x < outSize; x++) {
      const x0 = x * step, x1 = (x + 1) * step
      let r = 0, g = 0, b = 0, sum = 0
      for (let py = Math.floor(y0); py < Math.ceil(y1); py++) {
        const wy = Math.min(y1, py + 1) - Math.max(y0, py)
        if (wy <= 0) continue
        for (let px = Math.floor(x0); px < Math.ceil(x1); px++) {
          const wx = Math.min(x1, px + 1) - Math.max(x0, px)
          if (wx <= 0) continue
          const w = wx * wy
          const i = (py * cv.w + px) * 4
          r += cv.buf[i] * w
          g += cv.buf[i + 1] * w
          b += cv.buf[i + 2] * w
          sum += w
        }
      }
      const i = (y * outSize + x) * 4
      out[i] = Math.round(r / sum)
      out[i + 1] = Math.round(g / sum)
      out[i + 2] = Math.round(b / sum)
      out[i + 3] = 255
    }
  }
  return out
}

const swap565 = (r, g, b) => {
  const v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
  return ((v & 0xff) << 8) | (v >> 8)
}

function emitHeader(small) {
  const values = []
  for (let i = 0; i < OUT * OUT; i++) {
    values.push(swap565(small[i * 4], small[i * 4 + 1], small[i * 4 + 2]))
  }
  const lines = [
    '// Generated by tools/build_video_scene.mjs; 128x128, byte-swapped RGB565.',
    '// 主页"视频剧场"插画 (电视机 + 播放键), 底色 #f8dfd4 已烘焙。',
    '#pragma once',
    '#include <stdint.h>',
    'static const uint16_t HOME_VIDEO_SCENE[128 * 128] = {',
  ]
  for (let n = 0; n < values.length; n += 16) {
    lines.push('  ' + values.slice(n, n + 16).map((v) => `0x${v.toString(16).padStart(4, '0')}`).join(',') + ',')
  }
  lines.push('};')
  writeFileSync(HEADER, lines.join('\n') + '\n')
}

function main() {
  const cv = drawVideoScene()
  const workDir = mkdtempSync(join(tmpdir(), 'storybox-video-scene-'))
  try {
    const raw = join(workDir, 'scene.raw')
    writeFileSync(raw, cv.buf)
    execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y',
      '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', `${cv.w}x${cv.h}`, '-i', raw, PNG],
    { stdio: 'inherit' })
  } finally {
    rmSync(workDir, { recursive: true, force: true })
  }
  emitHeader(boxDownsample(cv, OUT))
  console.log(`scene:  ${PNG}`)
  console.log(`header: ${HEADER} (128x128 swap565)`)
}

main()
