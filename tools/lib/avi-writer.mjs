/**
 * 写一个最小可用的 AVI (RIFF) 容器, 只放一条视频流:
 *   RIFF('AVI ')
 *     LIST('hdrl') avih + LIST('strl'){ strh, strf }
 *     LIST('movi') '00dc' 帧...
 *
 * 与固件 src/video.cpp 的解析方式配套: 不写 idx1 (播放器自己扫 movi),
 * 帧率写在 strh 的 dwScale/dwRate, 总帧数在收尾时回填。
 *
 * 用法:
 *   const w = createAviWriter(path, { width, height, fps, codec: 'QOI ' })
 *   w.addFrame(buf); ...; w.finish()
 */
import { openSync, writeSync, fsyncSync, closeSync } from 'node:fs'

const pad4 = (n) => (n & 1 ? n + 1 : n)

class Cursor {
  constructor() {
    this.parts = []
    this.size = 0
  }
  raw(buf) { this.parts.push(buf); this.size += buf.length; return this }
  u32(v) {
    const b = Buffer.alloc(4)
    b.writeUInt32LE(v >>> 0, 0)
    return this.raw(b)
  }
  u16(v) {
    const b = Buffer.alloc(2)
    b.writeUInt16LE(v & 0xffff, 0)
    return this.raw(b)
  }
  tag(s) { return this.raw(Buffer.from(s, 'latin1')) }
  toBuffer() { return Buffer.concat(this.parts, this.size) }
}

export function createAviWriter(path, { width, height, fps, codec = 'QOI ' }) {
  if (!fps || fps <= 0) throw new Error('fps 必须 > 0')
  if (codec.length !== 4) throw new Error('codec 必须是 4 字节 fourcc')
  const fd = openSync(path, 'w')
  const usPerFrame = Math.round(1e6 / fps)

  const c = new Cursor()
  c.tag('RIFF').u32(0).tag('AVI ')
  const riffSizeOff = 4

  c.tag('LIST').u32(0).tag('hdrl')
  const hdrlSizeOff = c.size - 8
  const hdrlDataStart = c.size

  c.tag('avih').u32(56)
  const avihDataOff = c.size
  c.u32(usPerFrame)                    // dwMicroSecPerFrame
    .u32(0)                            // dwMaxBytesPerSec
    .u32(0)                            // dwPaddingGranularity
    .u32(0)                            // dwFlags
    .u32(0)                            // dwTotalFrames  (回填)
    .u32(0)                            // dwInitialFrames
    .u32(1)                            // dwStreams
    .u32(0)                            // dwSuggestedBufferSize
    .u32(width)
    .u32(height)
    .u32(0).u32(0).u32(0).u32(0)       // dwReserved[4]

  c.tag('LIST').u32(0).tag('strl')
  const strlSizeOff = c.size - 8
  const strlDataStart = c.size

  c.tag('strh').u32(56)
  const strhDataOff = c.size
  c.tag('vids').tag(codec)
    .u32(0)                            // dwFlags
    .u16(0).u16(0)                     // wPriority / wLanguage
    .u32(0)                            // dwInitialFrames
    .u32(1)                            // dwScale
    .u32(fps)                          // dwRate
    .u32(0)                            // dwStart
    .u32(0)                            // dwLength (回填)
    .u32(0)                            // dwSuggestedBufferSize
    .u32(0xffffffff)                   // dwQuality
    .u32(0)                            // dwSampleSize
    .u16(0).u16(0).u16(width).u16(height)   // rcFrame

  c.tag('strf').u32(40)
  const strfDataOff = c.size
  c.u32(40)                            // biSize
    .u32(width)
    .u32(height)
    .u16(1)                            // biPlanes
    .u16(24)                           // biBitCount
    .tag(codec)                        // biCompression
    .u32(0)                            // biSizeImage (回填为最大帧)
    .u32(0).u32(0)                     // biXPelsPerMeter / biYPelsPerMeter
    .u32(0).u32(0)                     // biClrUsed / biClrImportant

  const strlDataEnd = c.size
  const hdrlDataEnd = c.size

  c.tag('LIST').u32(0).tag('movi')
  const moviSizeOff = c.size - 8
  const moviDataStart = c.size
  const header = c.toBuffer()

  writeSync(fd, header)

  let frames = 0
  let bytes = header.length
  let maxFrame = 0
  const frameIndex = []        // [chunk 头偏移, 帧长度]

  return {
    addFrame(buf) {
      frameIndex.push([bytes, buf.length])
      const head = Buffer.alloc(8)
      head.write('00dc', 0, 'latin1')
      head.writeUInt32LE(buf.length, 4)
      writeSync(fd, head)
      writeSync(fd, buf)
      const padded = pad4(buf.length)
      if (padded > buf.length) writeSync(fd, Buffer.alloc(padded - buf.length))
      bytes += 8 + padded
      frames++
      if (buf.length > maxFrame) maxFrame = buf.length
    },
    finish() {
      const patch = (off, value) => {
        const b = Buffer.alloc(4)
        b.writeUInt32LE(value >>> 0, 0)
        writeSync(fd, b, 0, 4, off)
      }
      patch(hdrlSizeOff, hdrlDataEnd - (hdrlSizeOff + 4))
      patch(strlSizeOff, strlDataEnd - (strlSizeOff + 4))
      patch(moviSizeOff, bytes - (moviSizeOff + 4))
      patch(avihDataOff + 16, frames)      // dwTotalFrames
      patch(strhDataOff + 32, frames)      // dwLength
      patch(strfDataOff + 20, maxFrame)    // biSizeImage 用最大帧兜底

      /* 帧索引 (自定义 chunk, 播放器直接读, 不用逐帧扫 movi):
       *   'SBXI' | size | 'SBIX' | count | count * (u32 块头偏移, u32 帧长)
       * 标准播放器会忽略未知 chunk; 固件缺索引时回退到扫 movi。 */
      if (frames) {
        const size = 8 + frames * 8
        const chunk = Buffer.alloc(8 + size)
        chunk.write('SBXI', 0, 'latin1')
        chunk.writeUInt32LE(size, 4)
        chunk.write('SBIX', 8, 'latin1')
        chunk.writeUInt32LE(frames, 12)
        let o = 16
        for (const [off, len] of frameIndex) {
          chunk.writeUInt32LE(off, o)
          chunk.writeUInt32LE(len, o + 4)
          o += 8
        }
        writeSync(fd, chunk)
        bytes += chunk.length
      }

      patch(riffSizeOff, bytes - 8)
      fsyncSync(fd)
      closeSync(fd)
      return { frames, bytes, maxFrame }
    },
  }
}
