import { crc32 } from '@shared/transfer'
const MAGIC = Buffer.from('SBX4')
const HEADER = 16, MAX_JSON = 24576, MAX_DATA = 4096
export function encodeFrame(message: Record<string, unknown>, data: Buffer = Buffer.alloc(0)): Buffer {
  const json = Buffer.from(JSON.stringify(message), 'utf8')
  if (!json.length || json.length > MAX_JSON || data.length > MAX_DATA) throw new Error('FRAME_SIZE')
  const frame = Buffer.alloc(HEADER + json.length + data.length)
  MAGIC.copy(frame); frame.writeUInt16LE(json.length, 4); frame.writeUInt16LE(data.length, 6)
  json.copy(frame, HEADER); data.copy(frame, HEADER + json.length)
  frame.writeUInt32LE(crc32(frame.subarray(0, 8)), 8)
  frame.writeUInt32LE(crc32(frame.subarray(HEADER)), 12)
  return frame
}
export class FrameDecoder {
  private buffer = Buffer.alloc(0)
  private lastAt = 0
  constructor(private frame: (message: Record<string, unknown>, data: Buffer) => void,
    private log: (text: string) => void = () => {}) {}
  reset(): void { this.buffer = Buffer.alloc(0); this.lastAt = 0 }
  feed(chunk: Buffer): void {
    if (this.lastAt && Date.now() - this.lastAt > 2000) this.reset()
    this.lastAt = Date.now()
    this.buffer = Buffer.concat([this.buffer, chunk])
    while (this.buffer.length) {
      const start = this.buffer.indexOf(MAGIC)
      if (start < 0) {
        const end = this.buffer.lastIndexOf(10)
        if (end >= 0) { this.log(this.buffer.subarray(0,end).toString('utf8')); this.buffer=this.buffer.subarray(end+1) }
        if (this.buffer.length>4096) this.buffer=this.buffer.subarray(-3)
        return
      }
      if (start) { this.log(this.buffer.subarray(0,start).toString('utf8')); this.buffer=this.buffer.subarray(start) }
      if (this.buffer.length < HEADER) return
      const json = this.buffer.readUInt16LE(4), raw = this.buffer.readUInt16LE(6)
      if (!json || json>MAX_JSON || raw>MAX_DATA || this.buffer.readUInt32LE(8)!==crc32(this.buffer.subarray(0,8))) {
        this.buffer=this.buffer.subarray(1); continue
      }
      const size=HEADER+json+raw
      if (this.buffer.length<size) return
      const frame=this.buffer.subarray(0,size); this.buffer=this.buffer.subarray(size)
      if (frame.readUInt32LE(12)!==crc32(frame.subarray(HEADER))) { this.log('FRAME_CRC: 丢弃损坏帧'); continue }
      let message: Record<string,unknown>
      try { message=JSON.parse(frame.subarray(HEADER,HEADER+json).toString('utf8')) }
      catch { this.log('FRAME_JSON: 元数据无效'); continue }
      if (!message || typeof message !== 'object' || Array.isArray(message) || message.v !== 4) {
        this.log('FRAME_VERSION: 要求 v4 对象'); continue
      }
      this.frame(message, frame.subarray(HEADER+json))
    }
  }
}
