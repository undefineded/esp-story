import { describe, expect, it } from 'vitest'
import { encodeFrame, FrameDecoder } from './frame'
describe('SBX4 binary wire frames', () => {
  const doc={v:4,id:7,cmd:'fs',act:'write_chunk',offset:0}
  const raw=Buffer.from(Array.from({length:4096},(_,i)=>i%256))
  it('matches the cross-language little-endian wire vector', () => {
    expect(encodeFrame({}, Buffer.from([0,10,255])).toString('hex')).toBe('534258340200030088404758855998067b7d000aff')
  })
  it('round trips arbitrary bytes with every split position', () => {
    const frame=encodeFrame(doc,raw)
    for(let split=0;split<=frame.length;split++) {
      const result: Buffer[]=[]
      const parser=new FrameDecoder((message,data)=>{expect(message).toEqual(doc);result.push(data)})
      parser.feed(frame.subarray(0,split));parser.feed(frame.subarray(split))
      expect(result.length).toBe(1)
      expect(result[0].equals(raw)).toBe(true)
    }
  })
  it('handles byte-wise input, noise and concatenated frames', () => {
    const result:Buffer[]=[]
    const parser=new FrameDecoder((_,data)=>result.push(data))
    for(const byte of Buffer.concat([Buffer.from('[SYS] hello\n'),encodeFrame(doc,raw),encodeFrame(doc)])) parser.feed(Buffer.from([byte]))
    expect(result).toEqual([raw,Buffer.alloc(0)])
  })
  it('discards corrupt header/body and accepts the next good frame', () => {
    for(const pos of [4,8,12,20,200]) {
      const bad=encodeFrame(doc,raw);bad[pos]^=255
      const result:Buffer[]=[]
      const parser=new FrameDecoder((_,data)=>result.push(data))
      parser.feed(Buffer.concat([bad,encodeFrame(doc,raw)]))
      expect(result).toEqual([raw])
    }
  })
  it('rejects oversized raw payloads',()=>expect(()=>encodeFrame(doc,Buffer.alloc(4097))).toThrow('FRAME_SIZE'))
})
