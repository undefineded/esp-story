import { describe, expect, it, vi } from 'vitest'
vi.mock('serialport', () => ({ SerialPort: class {} }))
import { NfcSession } from './serial'
import type { SettingsStore } from './settings'

function fixture() {
  const session = new NfcSession({} as SettingsStore)
  const internals = session as unknown as {
    request: (cmd: string, payload: Record<string, unknown>, timeout?: number, quiet?: boolean, raw?: Buffer) => Promise<Record<string, unknown>>
  }
  const request = vi.spyOn(internals, 'request')
  return { session, request }
}
describe('serial file transfer', () => {
  it('pipelines four raw blocks without text encoding', async () => {
    const { session, request } = fixture()
    let inFlight = 0, maximum = 0
    request.mockImplementation(async (_, p, _timeout, _quiet, bytes) => {
      expect(Buffer.isBuffer(bytes)).toBe(true)
      expect(p.data).toBeUndefined()
      maximum = Math.max(maximum, ++inFlight)
      await Promise.resolve()
      inFlight--
      return { data: { written: Number(p.offset) + bytes!.length } }
    })
    expect(await session.fsWriteChunk(1, 0, new Uint8Array(40000))).toEqual({ written: 40000 })
    expect(maximum).toBe(4)
    expect(request).toHaveBeenCalledTimes(10)
  })
  it('preserves the last partial raw block', async () => {
    const { session, request } = fixture()
    request.mockImplementation(async (_, p, _timeout, _quiet, bytes) => {
      return { data: { written: Number(p.offset) + bytes!.length } }
    })
    expect(session.transferWindowSize()).toBe(4)
    expect(await session.fsWriteChunk(1, 0, new Uint8Array(5000))).toEqual({ written: 5000 })
  })
  it('does not invent successful acknowledgement on a short write', async () => {
    const { session, request } = fixture()
    request.mockResolvedValue({ data: { written: 0 } })
    await expect(session.fsWriteChunk(1, 0, new Uint8Array(16))).rejects.toThrow('VERIFY')
  })
  it('rejects download data with a length mismatch', async () => {
    const { session, request } = fixture()
    request.mockResolvedValue({ data: { offset: 0, len: 1, bytes: Buffer.alloc(0) } })
    await expect(session.fsReadChunk(1, 0)).rejects.toThrow('VERIFY')
  })
  it('prevents overlapping sessions and releases its local gate after end failure', async () => {
    const { session, request } = fixture()
    request.mockResolvedValue({})
    await session.fsSessionBegin()
    await expect(session.fsSessionBegin()).rejects.toThrow('BUSY')
    request.mockRejectedValueOnce(new Error('disconnected'))
    await expect(session.fsSessionEnd()).rejects.toThrow('disconnected')
    await expect(session.fsSessionBegin()).resolves.toBeUndefined()
  })
})
