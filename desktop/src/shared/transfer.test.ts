import { describe, expect, it } from 'vitest'
import { crc32, transferWindow } from './transfer'

describe('file transfer primitives', () => {
  it('matches IEEE CRC32 standard vectors', () => {
    expect(crc32(new Uint8Array())).toBe(0)
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926)
  })
  it('preserves file order even when acknowledgements arrive out of order', async () => {
    let finish!: (value: number) => void
    const result = transferWindow([() => new Promise<number>((r) => { finish = r }), async () => 2])
    await Promise.resolve()
    finish(1)
    expect(await result).toEqual([1, 2])
  })
  it('drains in-flight requests before reporting failure, so cleanup cannot race them', async () => {
    let finish!: () => void
    let settled = false
    const result = transferWindow([
      async () => { throw new Error('CRC') },
      () => new Promise<void>((r) => { finish = r })
    ]).catch((e: Error) => { settled = true; return e.message })
    await Promise.resolve(); await Promise.resolve(); await Promise.resolve()
    expect(settled).toBe(false)
    finish()
    expect(await result).toBe('CRC')
  })
})
