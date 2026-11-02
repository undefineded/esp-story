/**
 * 读 AVI 头 + 扫 movi 列表, 给 tools/build_video.mjs / tools/verify_video.mjs 共用。
 * 逻辑与固件 src/video.cpp 的索引方式一致: 只看 '00dc'/'00db' 帧块, 忽略 idx1。
 */
import { openSync, readSync, closeSync, statSync } from 'node:fs'

const readAt = (fd, pos, len) => {
  const buf = Buffer.alloc(len)
  const n = readSync(fd, buf, 0, len, pos)
  return n === len ? buf : buf.subarray(0, n)
}

const tag = (buf, off = 0) => buf.toString('latin1', off, off + 4)

export function parseAvi(path, { maxFrames = 1_000_000 } = {}) {
  const size = statSync(path).size
  const fd = openSync(path, 'r')
  try {
    const hdr = readAt(fd, 0, 12)
    if (tag(hdr) !== 'RIFF' || tag(hdr, 8) !== 'AVI ') {
      throw new Error('不是 AVI (RIFF/AVI 标记缺失)')
    }

    let avih = null
    let video = { mjpeg: false, codec: '', width: 0, height: 0, frameUs: 0 }
    let hasAudioStream = false
    let moviStart = 0
    let moviEnd = 0
    let sbxi = null

    const parseStrh = (pos, chunkSize) => {
      const b = readAt(fd, pos, Math.min(chunkSize, 56))
      const type = tag(b)
      if (type === 'vids') {
        const scale = b.readUInt32LE(20)
        const rate = b.readUInt32LE(24)
        if (rate > 0 && scale > 0) video.frameUs = Math.round((scale * 1e6) / rate)
      } else if (type === 'auds') {
        hasAudioStream = true
      }
    }

    const parseStrf = (pos, chunkSize, isVideo) => {
      if (!isVideo) return
      const b = readAt(fd, pos, Math.min(chunkSize, 40))
      video.width = b.readUInt32LE(4)
      video.height = b.readUInt32LE(8)
      video.codec = tag(b, 16)
      video.mjpeg = video.codec.toUpperCase() === 'MJPG'
    }

    const parseStrl = (start, end) => {
      let pos = start
      let isVideo = false
      while (pos + 8 <= end) {
        const h = readAt(fd, pos, 8)
        const id = tag(h)
        const sz = h.readUInt32LE(4)
        const dataEnd = pos + 8 + sz
        if (dataEnd > end) return
        if (id === 'strh') {
          const b = readAt(fd, pos + 8, Math.min(sz, 56))
          isVideo = tag(b) === 'vids'
          parseStrh(pos + 8, sz)
        } else if (id === 'strf') {
          parseStrf(pos + 8, sz, isVideo)
        }
        pos = dataEnd + (sz & 1)
      }
    }

    const parseHdrl = (start, end) => {
      let pos = start
      while (pos + 8 <= end) {
        const h = readAt(fd, pos, 8)
        const id = tag(h)
        const sz = h.readUInt32LE(4)
        const dataEnd = pos + 8 + sz
        if (dataEnd > end + 1) return
        if (id === 'LIST') {
          const t = tag(readAt(fd, pos + 8, 4))
          if (t === 'strl') parseStrl(pos + 12, dataEnd)
          else if (t === 'hdrl') parseHdrl(pos + 12, dataEnd)
        } else if (id === 'avih') {
          const b = readAt(fd, pos + 8, Math.min(sz, 56))
          avih = {
            usPerFrame: b.readUInt32LE(0),
            totalFrames: b.readUInt32LE(16),
            width: b.readUInt32LE(32),
            height: b.readUInt32LE(36),
          }
        }
        pos = dataEnd + (sz & 1)
      }
    }

    let pos = 12
    while (pos + 8 <= size) {
      const h = readAt(fd, pos, 8)
      if (h.length < 8) break
      const id = tag(h)
      const sz = h.readUInt32LE(4)
      const next = pos + 8 + sz + (sz & 1)
      if (next > size + 1) break
      if (id === 'LIST') {
        const t = tag(readAt(fd, pos + 8, 4))
        if (t === 'hdrl') parseHdrl(pos + 12, pos + 8 + sz)
        else if (t === 'movi') {
          moviStart = pos + 12
          moviEnd = pos + 8 + sz
        }
      } else if (id === 'SBXI') {
        sbxi = { pos, size: sz }
      }
      pos = next
    }

    if (!moviStart) throw new Error('AVI 里没有 movi 列表')
    if (!video.codec) throw new Error('缺少视频流信息 (strf)')

    /* 转码时写入的帧索引: 固件优先读它, 不用逐帧扫 movi */
    let indexed = false
    if (sbxi && sbxi.size >= 16) {
      const ih = readAt(fd, sbxi.pos, 16)
      if (tag(ih) === 'SBXI' && tag(ih, 8) === 'SBIX') {
        const count = ih.readUInt32LE(12)
        indexed = count > 0 && sbxi.size === 8 + count * 8
      }
    }

    const frames = []
    const scan = (start, end, depth) => {
      let p = start
      while (p + 8 <= end) {
        const h = readAt(fd, p, 8)
        if (h.length < 8) return
        const id = tag(h)
        const sz = h.readUInt32LE(4)
        const dataEnd = p + 8 + sz
        if (dataEnd > end) return
        if (id === 'LIST') {
          if (depth < 2) scan(p + 12, dataEnd, depth + 1)
        } else if (id === '00dc' || id === '00db') {
          if (frames.length >= maxFrames) throw new Error(`帧数超过上限 ${maxFrames}`)
          frames.push({ offset: p + 8, size: sz })
        }
        p = dataEnd + (sz & 1)
      }
    }
    scan(moviStart, moviEnd, 0)

    const frameUs = video.frameUs || avih?.usPerFrame || 40000
    return {
      size,
      codec: video.codec,
      width: video.width || avih?.width || 0,
      height: video.height || avih?.height || 0,
      frameUs,
      fps: Math.round(1e6 / frameUs),
      frames,
      hasAudioStream,
      indexed,
      durationMs: Math.round((frames.length * frameUs) / 1000),
    }
  } finally {
    closeSync(fd)
  }
}
