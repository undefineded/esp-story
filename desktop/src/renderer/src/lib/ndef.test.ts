import { describe, expect, it } from 'vitest'
import {
  buildNdefMessage,
  buildType2UserData,
  bytesToHex,
  describeNdef,
  estimateTlvSize,
  extractNdefMessage,
  hexToBytes,
  parseCardNdef,
  parseNdefMessage
} from './ndef'

describe('hex helpers', () => {
  it('round-trips bytes', () => {
    const bytes = Uint8Array.from([0x00, 0x01, 0xab, 0xff])
    expect(bytesToHex(bytes)).toBe('0001ABFF')
    expect(Array.from(hexToBytes('00 01 ab ff'))).toEqual([0, 1, 0xab, 0xff])
  })

  it('rejects odd hex', () => {
    expect(() => hexToBytes('abc')).toThrow()
  })
})

describe('NDEF text records', () => {
  it('builds and parses UTF-8 Chinese text', () => {
    const message = buildNdefMessage([{ kind: 'text', text: '月亮邮递员 · 第 03 集', language: 'zh' }])
    const parsed = parseNdefMessage(message)
    expect(parsed.error).toBeUndefined()
    expect(parsed.records).toHaveLength(1)
    expect(parsed.records[0].kind).toBe('text')
    expect(parsed.records[0].value).toBe('月亮邮递员 · 第 03 集')
    expect(parsed.records[0].language).toBe('zh')
    expect(parsed.records[0].tnf).toBe(0x01)
    expect(parsed.records[0].type).toBe('T')
  })

  it('decodes UTF-16 text records', () => {
    const language = new TextEncoder().encode('en')
    const text = new TextEncoder().encode('Hi')
    // UTF-16BE "你好"
    const utf16 = Uint8Array.from([0x4f, 0x60, 0x59, 0x7d])
    const payload = new Uint8Array(1 + language.length + utf16.length)
    payload[0] = 0x80 | language.length
    payload.set(language, 1)
    payload.set(utf16, 1 + language.length)
    const type = new TextEncoder().encode('T')
    const record = new Uint8Array(4 + type.length + payload.length)
    record[0] = 0xd1
    record[1] = type.length
    record[2] = payload.length
    record[3] = type[0]
    record.set(payload, 4)
    const parsed = parseNdefMessage(record)
    expect(parsed.records[0].value).toBe('你好')
    expect(parsed.records[0].language).toBe('en')
    expect(text.length).toBe(2) // keep the fixture honest
  })

  it('handles payloads longer than 255 bytes', () => {
    const text = 'A'.repeat(300)
    const message = buildNdefMessage([{ kind: 'text', text, language: 'en' }])
    expect(message[0] & 0x10).toBe(0) // short-record flag is off
    const parsed = parseNdefMessage(message)
    expect(parsed.records[0].value).toBe(text)
  })
})

describe('NDEF URI records', () => {
  it('compresses https://www. prefix', () => {
    const message = buildNdefMessage([{ kind: 'uri', uri: 'https://www.example.com/story' }])
    const parsed = parseNdefMessage(message)
    expect(parsed.records[0].kind).toBe('uri')
    expect(parsed.records[0].value).toBe('https://www.example.com/story')
    const payload = hexToBytes(parsed.records[0].payloadHex)
    expect(payload[0]).toBe(0x02)
  })

  it('keeps unknown schemes as-is', () => {
    const message = buildNdefMessage([{ kind: 'uri', uri: 'storybox://card/42' }])
    const parsed = parseNdefMessage(message)
    expect(parsed.records[0].value).toBe('storybox://card/42')
    expect(hexToBytes(parsed.records[0].payloadHex)[0]).toBe(0x00)
  })
})

describe('Type 2 TLV', () => {
  it('extracts NDEF after memory-control TLV', () => {
    const message = buildNdefMessage([{ kind: 'text', text: 'hello', language: 'en' }])
    const user = new Uint8Array(4 + 2 + message.length + 1)
    // TLV 0x02 memory control, length 3
    user.set([0x02, 0x03, 0xaa, 0xbb, 0xcc, 0x03], 0)
    user[6] = message.length
    user.set(message, 7)
    user[7 + message.length] = 0xfe
    const result = extractNdefMessage(user)
    expect(result.ok).toBe(true)
    expect(result.formatted).toBe(true)
    expect(bytesToHex(result.message)).toBe(bytesToHex(message))
  })

  it('reports a formatted tag without an NDEF TLV', () => {
    const result = extractNdefMessage(Uint8Array.from([0x00, 0xfe]))
    expect(result.ok).toBe(false)
    expect(result.error).toMatch(/没有 NDEF/)
  })

  it('reports a truncated NDEF TLV', () => {
    const result = extractNdefMessage(Uint8Array.from([0x03, 0x20, 0x01, 0x02]))
    expect(result.ok).toBe(false)
    expect(result.formatted).toBe(true)
  })
})

describe('Type 2 card helpers', () => {
  it('builds user data padded to capacity', () => {
    const message = buildNdefMessage([{ kind: 'text', text: 'hi', language: 'en' }])
    const user = buildType2UserData(message, 144)
    expect(user.length).toBe(144)
    expect(user[0]).toBe(0x03)
    expect(user[1]).toBe(message.length)
    expect(user[2 + message.length]).toBe(0xfe)
    expect(user.slice(3 + message.length).every((b) => b === 0)).toBe(true)
  })

  it('refuses data larger than capacity', () => {
    const message = buildNdefMessage([{ kind: 'text', text: 'x'.repeat(200), language: 'en' }])
    expect(() => buildType2UserData(message, 144)).toThrow(/超过/)
  })

  it('parses a card snapshot', () => {
    const message = buildNdefMessage([{ kind: 'text', text: '故事卡片', language: 'zh' }])
    const data = bytesToHex(buildType2UserData(message, 144))
    const parsed = parseCardNdef({ formatted: true, data })
    expect(parsed.ok).toBe(true)
    expect(parsed.records[0].value).toBe('故事卡片')
    expect(describeNdef(parsed)).toBe('故事卡片')
  })

  it('marks unformatted cards', () => {
    const parsed = parseCardNdef({ formatted: false, data: '' })
    expect(parsed.ok).toBe(false)
    expect(parsed.formatted).toBe(false)
  })
})

describe('estimates', () => {
  it('matches the TLV size', () => {
    expect(estimateTlvSize(10)).toBe(13)
    expect(estimateTlvSize(300)).toBe(305)
  })
})
