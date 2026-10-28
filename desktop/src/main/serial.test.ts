import { describe, expect, it, vi } from 'vitest'
vi.mock('serialport', () => ({ SerialPort: class {} }))
import { NfcSession } from './serial'
import type { SettingsStore } from './settings'
import { crc32 } from '@shared/transfer'

function fixture(features: string[] = []) {
  const session = new NfcSession({} as SettingsStore)
  const internals = session as unknown as {
    device: { features: string[] }
    request: (cmd: string, payload: Record<string, unknown>) => Promise<Record<string, unknown>>
  }
  internals.device = { features }
  const request = vi.spyOn(internals, 'request')
  return { session, request }
}
describe('serial file transfer', () => {
  it('pipelines four blocks on capable firmware and validates CRC data', async () => {
    const { session, request } = fixture(['fs.window4.crc32'])
    let inFlight = 0, maximum = 0
    request.mockImplementation(async (_, p) => {
      const bytes = Buffer.from(String(p.data), 'base64')
      expect(p.crc32).toBe(crc32(bytes))
      maximum = Math.max(maximum, ++inFlight)
      await Promise.resolve()
      inFlight--
      return { data: { written: Number(p.offset) + bytes.length } }
    })
    expect(await session.fsWriteChunk(1, 0, new Uint8Array(40000))).toEqual({ written: 40000 })
    expect(maximum).toBe(4)
    expect(request).toHaveBeenCalledTimes(10)
  })
  it('keeps single-block requests for legacy firmware', async () => {
    const { session, request } = fixture()
    request.mockImplementation(async (_, p) => {
      expect(p.crc32).toBeUndefined()
      return { data: { written: Number(p.offset) + Buffer.from(String(p.data), 'base64').length } }
    })
    expect(session.transferWindowSize()).toBe(1)
    expect(await session.fsWriteChunk(1, 0, new Uint8Array(5000))).toEqual({ written: 5000 })
  })
  it('does not invent successful acknowledgement on a short write', async () => {
    const { session, request } = fixture()
    request.mockResolvedValue({ data: { written: 0 } })
    await expect(session.fsWriteChunk(1, 0, new Uint8Array(16))).rejects.toThrow('VERIFY')
  })
  it('rejects corrupted download data', async () => {
    const { session, request } = fixture()
    request.mockResolvedValue({ data: { offset: 0, len: 1, data: 'AA==', crc32: 123 } })
    await expect(session.fsReadChunk(1, 0)).rejects.toThrow('VERIFY')
  })
  it('prevents overlapping sessions and releases its local gate after end failure', async () => {
    const { session, request } = fixture(['fs.session'])
    request.mockResolvedValue({})
    await session.fsSessionBegin()
    await expect(session.fsSessionBegin()).rejects.toThrow('BUSY')
    request.mockRejectedValueOnce(new Error('disconnected'))
    await expect(session.fsSessionEnd()).rejects.toThrow('disconnected')
    await expect(session.fsSessionBegin()).resolves.toBeUndefined()
  })
})
