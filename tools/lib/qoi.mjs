/**
 * QOI (Quite OK Image) 编解码, 供视频转码脚本使用。
 *
 * 为什么不用 JPEG: 设备端软解的大头是 Huffman + IDCT + 色度上采样 (320x240
 * 约 20~35ms), 而 QOI 每像素只有查表/差分/RLE, 解码约 3~6ms, 体积约 1.5~3 倍
 * (优先级放低后完全可接受)。
 *
 * 设备端解码用 LovyanGFX 自带的 QOI 解码器 (lgfx_qoi.c), 这里只负责编码,
 * 以及给自测用的解码。
 *
 * 规范: https://qoiformat.org/qoi-specification.pdf
 *   header: 'qoif' u32 width u32 height u8 channels u8 colorspace
 *   pixel:  INDEX(0x00) DIFF(0x40) LUMA(0x80) RUN(0xc0) RGB(0xfe) RGBA(0xff)
 *   end:    7 x 0x00 + 0x01
 */

const QOI_OP_INDEX = 0x00
const QOI_OP_DIFF = 0x40
const QOI_OP_LUMA = 0x80
const QOI_OP_RUN = 0xc0
const QOI_OP_RGB = 0xfe
const QOI_OP_RGBA = 0xff

const QOI_HEADER_SIZE = 14
const QOI_PADDING = Buffer.from([0, 0, 0, 0, 0, 0, 0, 1])

/** 把 RGB888 量化到 RGB565 再展开: 与设备 16bpp 画布完全一致, 还能压得更小 */
export function quantize565(rgb, width, height) {
  const out = Buffer.alloc(width * height * 3)
  for (let i = 0, n = width * height * 3; i < n; i += 3) {
    const r5 = rgb[i] >> 3
    const g6 = rgb[i + 1] >> 2
    const b5 = rgb[i + 2] >> 3
    out[i] = (r5 << 3) | (r5 >> 2)
    out[i + 1] = (g6 << 2) | (g6 >> 4)
    out[i + 2] = (b5 << 3) | (b5 >> 2)
  }
  return out
}

/**
 * 编码一帧 RGB888 (长度 width*height*3)。
 * channels=3 时不写 alpha, 设备端按不透明处理。
 */
export function encodeQoi(rgb, width, height) {
  if (rgb.length !== width * height * 3) {
    throw new Error(`encodeQoi: 需要 ${width * height * 3} 字节, 收到 ${rgb.length}`)
  }
  const maxSize = width * height * 5 + QOI_HEADER_SIZE + QOI_PADDING.length
  const out = Buffer.allocUnsafe(maxSize)
  out.write('qoif', 0, 'latin1')
  out.writeUInt32BE(width, 4)
  out.writeUInt32BE(height, 8)
  out[12] = 3        // channels: RGB
  out[13] = 0        // colorspace: sRGB
  let p = QOI_HEADER_SIZE

  const index = new Int32Array(64)   // 打包的 RGB
  let prevR = 0, prevG = 0, prevB = 0
  let run = 0
  const pxEnd = width * height * 3

  for (let i = 0; i < pxEnd; i += 3) {
    const r = rgb[i], g = rgb[i + 1], b = rgb[i + 2]
    if (r === prevR && g === prevG && b === prevB) {
      run++
      if (run === 62 || i + 3 === pxEnd) {
        out[p++] = QOI_OP_RUN | (run - 1)
        run = 0
      }
      continue
    }
    if (run > 0) {
      out[p++] = QOI_OP_RUN | (run - 1)
      run = 0
    }

    const packed = (r << 16) | (g << 8) | b
    const idx = (r * 3 + g * 5 + b * 7 + 255 * 11) & 63
    if (index[idx] === packed) {
      out[p++] = QOI_OP_INDEX | idx
    } else {
      index[idx] = packed
      const dr = r - prevR, dg = g - prevG, db = b - prevB
      if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
        out[p++] = QOI_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)
      } else {
        const drdg = dr - dg
        const dbdg = db - dg
        if (drdg >= -8 && drdg <= 7 && dg >= -32 && dg <= 31 && dbdg >= -8 && dbdg <= 7) {
          out[p++] = QOI_OP_LUMA | (dg + 32)
          out[p++] = ((drdg + 8) << 4) | (dbdg + 8)
        } else {
          out[p++] = QOI_OP_RGB
          out[p++] = r
          out[p++] = g
          out[p++] = b
        }
      }
    }
    prevR = r; prevG = g; prevB = b
  }

  QOI_PADDING.copy(out, p)
  p += QOI_PADDING.length
  return out.subarray(0, p)
}

/** 解码 (自测用): 返回 { width, height, channels, rgb } */
export function decodeQoi(buf) {
  if (buf.length < QOI_HEADER_SIZE + 8 || buf.toString('latin1', 0, 4) !== 'qoif') {
    throw new Error('不是 QOI')
  }
  const width = buf.readUInt32BE(4)
  const height = buf.readUInt32BE(8)
  const channels = buf[12]
  if (channels !== 3 && channels !== 4) throw new Error(`QOI channels=${channels}`)
  const rgb = Buffer.alloc(width * height * 3)
  const index = new Int32Array(64)
  let r = 0, g = 0, b = 0, a = 255
  let p = QOI_HEADER_SIZE
  let run = 0
  let out = 0

  while (out < width * height) {
    if (run > 0) { run-- }
    else {
      const b1 = buf[p++]
      if (b1 === QOI_OP_RGB) { r = buf[p++]; g = buf[p++]; b = buf[p++] }
      else if (b1 === QOI_OP_RGBA) { r = buf[p++]; g = buf[p++]; b = buf[p++]; a = buf[p++] }
      else if ((b1 & 0xc0) === QOI_OP_INDEX) {
        const v = index[b1 & 0x3f]
        r = (v >> 16) & 0xff; g = (v >> 8) & 0xff; b = v & 0xff
      } else if ((b1 & 0xc0) === QOI_OP_DIFF) {
        r = (r + ((b1 >> 4) & 3) - 2) & 0xff
        g = (g + ((b1 >> 2) & 3) - 2) & 0xff
        b = (b + (b1 & 3) - 2) & 0xff
      } else if ((b1 & 0xc0) === QOI_OP_LUMA) {
        const b2 = buf[p++]
        const dg = (b1 & 0x3f) - 32
        r = (r + dg + ((b2 >> 4) & 0x0f) - 8) & 0xff
        g = (g + dg) & 0xff
        b = (b + dg + (b2 & 0x0f) - 8) & 0xff
      } else {
        run = b1 & 0x3f
      }
      index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = (r << 16) | (g << 8) | b
    }
    rgb[out * 3] = r
    rgb[out * 3 + 1] = g
    rgb[out * 3 + 2] = b
    out++
  }
  return { width, height, channels, rgb }
}
