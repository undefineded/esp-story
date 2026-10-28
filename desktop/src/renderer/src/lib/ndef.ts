/* NDEF / Type 2 Tag 工具: 与固件解耦, 上位机负责构造和解析 NDEF 消息。
 * 固件只接收 NDEF 原始字节, 再包上 TLV 写到 NTAG 用户区。
 */
import type { NdefKind, NdefParse, NdefRecord, RawCard } from '@shared/types'

const URI_PREFIXES: Array<[number, string]> = [
  [0x01, 'http://www.'],
  [0x02, 'https://www.'],
  [0x03, 'http://'],
  [0x04, 'https://'],
  [0x05, 'tel:'],
  [0x06, 'mailto:'],
  [0x07, 'ftp://anonymous:anonymous@'],
  [0x08, 'ftp://ftp.'],
  [0x09, 'ftps://'],
  [0x0a, 'sftp://'],
  [0x0b, 'smb://'],
  [0x0c, 'nfs://'],
  [0x0d, 'ftp://'],
  [0x0e, 'dav://'],
  [0x0f, 'news:'],
  [0x10, 'telnet://'],
  [0x11, 'imap:'],
  [0x12, 'rtsp://'],
  [0x13, 'urn:'],
  [0x14, 'pop:'],
  [0x15, 'sip:'],
  [0x16, 'sips:'],
  [0x17, 'tftp:'],
  [0x18, 'btspp://'],
  [0x19, 'btl2cap://'],
  [0x1a, 'btgoep://'],
  [0x1b, 'tcpobex://'],
  [0x1c, 'irdaobex://'],
  [0x1d, 'file://'],
  [0x1e, 'urn:epc:id:'],
  [0x1f, 'urn:epc:tag:'],
  [0x20, 'urn:epc:pat:'],
  [0x21, 'urn:epc:raw:'],
  [0x22, 'urn:epc:'],
  [0x23, 'urn:nfc:']
]

export type RecordInput =
  | { kind: 'text'; text: string; language?: string }
  | { kind: 'uri'; uri: string }
  | { kind: 'custom'; tnf: number; type: string; payloadHex: string }

const encoder = new TextEncoder()
const decoder = new TextDecoder('utf-8')

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
}

export function hexToBytes(hex: string): Uint8Array {
  const clean = hex.replace(/[^0-9a-fA-F]/g, '')
  if (clean.length % 2 !== 0) throw new Error('十六进制长度必须为偶数')
  const out = new Uint8Array(clean.length / 2)
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16)
  }
  return out
}

function utf8(text: string): Uint8Array {
  return encoder.encode(text)
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((sum, p) => sum + p.length, 0)
  const out = new Uint8Array(total)
  let offset = 0
  for (const part of parts) {
    out.set(part, offset)
    offset += part.length
  }
  return out
}

function encodeTextPayload(text: string, language = 'zh'): Uint8Array {
  const lang = utf8((language || 'zh').slice(0, 63))
  const body = utf8(text)
  const status = lang.length & 0x3f
  return concat([new Uint8Array([status]), lang, body])
}

function pickUriPrefix(uri: string): { code: number; rest: string } {
  let best: { code: number; prefix: string } | null = null
  for (const [code, prefix] of URI_PREFIXES) {
    if (uri.startsWith(prefix) && (!best || prefix.length > best.prefix.length)) {
      best = { code, prefix }
    }
  }
  if (best) return { code: best.code, rest: uri.slice(best.prefix.length) }
  return { code: 0x00, rest: uri }
}

function encodeUriPayload(uri: string): Uint8Array {
  const { code, rest } = pickUriPrefix(uri.trim())
  return concat([new Uint8Array([code]), utf8(rest)])
}

function encodeRecord(input: RecordInput, first: boolean, last: boolean): Uint8Array {
  let tnf = 0x01
  let type: Uint8Array
  let payload: Uint8Array

  if (input.kind === 'text') {
    type = utf8('T')
    payload = encodeTextPayload(input.text, input.language)
  } else if (input.kind === 'uri') {
    type = utf8('U')
    payload = encodeUriPayload(input.uri)
  } else {
    tnf = input.tnf & 0x07
    type = utf8(input.type)
    payload = hexToBytes(input.payloadHex)
  }

  if (type.length > 255) throw new Error('NDEF type 过长')
  if (payload.length > 0xffffffff) throw new Error('NDEF payload 过长')

  const short = payload.length < 256
  const flags =
    (first ? 0x80 : 0) |
    (last ? 0x40 : 0) |
    (short ? 0x10 : 0) |
    tnf

  const out: number[] = [flags, type.length]
  if (short) {
    out.push(payload.length)
  } else {
    const len = payload.length
    out.push((len >>> 24) & 0xff, (len >>> 16) & 0xff, (len >>> 8) & 0xff, len & 0xff)
  }
  out.push(...type)
  out.push(...payload)
  return Uint8Array.from(out)
}

/** 构造一条完整的 NDEF 消息 (每条记录都是短记录或普通记录) */
export function buildNdefMessage(records: RecordInput[]): Uint8Array {
  if (records.length === 0) return new Uint8Array(0)
  return concat(records.map((record, i) => encodeRecord(record, i === 0, i === records.length - 1)))
}

export interface TlvResult {
  ok: boolean
  formatted: boolean
  message: Uint8Array
  tlvLength: number
  error?: string
}

/** 从 Type 2 用户区数据中提取 NDEF TLV (跳过 Lock/Memory Control TLV) */
export function extractNdefMessage(userData: Uint8Array): TlvResult {
  let i = 0
  while (i < userData.length) {
    const type = userData[i]
    if (type === 0x00) {
      i++
      continue
    }
    if (type === 0xfe) {
      return { ok: false, formatted: false, message: new Uint8Array(0), tlvLength: i, error: '标签中没有 NDEF TLV' }
    }
    if (type === 0x01 || type === 0x02) {
      if (i + 1 >= userData.length) break
      const len = userData[i + 1]
      i += 2 + len
      continue
    }
    if (type === 0xfd) {
      if (i + 1 >= userData.length) break
      const len = userData[i + 1]
      i += 2 + len
      continue
    }
    if (type !== 0x03) {
      return { ok: false, formatted: false, message: new Uint8Array(0), tlvLength: i, error: `未知 TLV 0x${type.toString(16)}` }
    }
    if (i + 1 >= userData.length) break
    let len = userData[i + 1]
    let start = i + 2
    let lengthBytes = 1
    if (len === 0xff) {
      if (i + 3 >= userData.length) break
      len = (userData[i + 2] << 8) | userData[i + 3]
      start = i + 4
      lengthBytes = 3
    }
    if (start + len > userData.length) {
      return {
        ok: false,
        formatted: true,
        message: new Uint8Array(0),
        tlvLength: i + 1 + lengthBytes,
        error: 'NDEF 消息超出已读取的数据范围'
      }
    }
    return {
      ok: true,
      formatted: true,
      message: userData.slice(start, start + len),
      tlvLength: i + 1 + lengthBytes + len
    }
  }
  return { ok: false, formatted: false, message: new Uint8Array(0), tlvLength: i, error: '没有找到 NDEF 记录' }
}

function uriFromPayload(payload: Uint8Array): string {
  if (payload.length === 0) return ''
  const code = payload[0]
  const prefix = URI_PREFIXES.find(([value]) => value === code)?.[1] ?? ''
  return prefix + decoder.decode(payload.slice(1))
}

function decodeUtf16Be(bytes: Uint8Array): string {
  const swapped = new Uint8Array(bytes.length)
  for (let i = 0; i + 1 < bytes.length; i += 2) {
    swapped[i] = bytes[i + 1]
    swapped[i + 1] = bytes[i]
  }
  return new TextDecoder('utf-16le').decode(swapped)
}

function decodeTextPayload(payload: Uint8Array): { value: string; language: string } {
  if (payload.length === 0) return { value: '', language: '' }
  const status = payload[0]
  const langLen = status & 0x3f
  const utf16 = (status & 0x80) !== 0
  const language = decoder.decode(payload.slice(1, 1 + langLen))
  const body = payload.slice(1 + langLen)
  return { value: utf16 ? decodeUtf16Be(body) : decoder.decode(body), language }
}

function decodePayloadText(payload: Uint8Array): string {
  const text = decoder.decode(payload)
  // 可打印字符占比高时按文本显示, 否则以 hex 呈现
  const printable = Array.from(text).filter((c) => c >= ' ' || c === '\n' || c === '\t').length
  if (text.length > 0 && printable / text.length > 0.85) return text
  return bytesToHex(payload)
}

function kindLabel(kind: NdefKind): string {
  switch (kind) {
    case 'text': return '文本'
    case 'uri': return '链接'
    case 'mime': return 'MIME'
    case 'external': return '扩展记录'
    case 'empty': return '空记录'
    default: return '未知'
  }
}

let recordCounter = 0

function parseRecord(tnf: number, type: Uint8Array, payload: Uint8Array): NdefRecord {
  const typeText = decoder.decode(type)
  let kind: NdefKind = 'unknown'
  let value = bytesToHex(payload)
  let language: string | undefined
  let uri: string | undefined

  if (tnf === 0x01 && typeText === 'T') {
    kind = 'text'
    const decoded = decodeTextPayload(payload)
    value = decoded.value
    language = decoded.language
  } else if (tnf === 0x01 && typeText === 'U') {
    kind = 'uri'
    uri = uriFromPayload(payload)
    value = uri
  } else if (tnf === 0x02) {
    kind = 'mime'
    value = decodePayloadText(payload)
  } else if (tnf === 0x04) {
    kind = 'external'
    value = decodePayloadText(payload)
  } else if (tnf === 0x00 && type.length === 0 && payload.length === 0) {
    kind = 'empty'
    value = ''
  }

  return {
    id: `rec-${++recordCounter}`,
    tnf,
    type: typeText,
    kind,
    typeLabel: kindLabel(kind),
    value,
    language,
    uri,
    payloadHex: bytesToHex(payload)
  }
}

export interface ParsedNdef {
  records: NdefRecord[]
  error?: string
}

/** 解析 NDEF 消息; 只支持非分块或简单分块记录, 足以覆盖文本/链接/自定义数据 */
export function parseNdefMessage(message: Uint8Array): ParsedNdef {
  const records: NdefRecord[] = []
  let offset = 0
  let guard = 0
  while (offset < message.length) {
    if (++guard > 256) return { records, error: 'NDEF 记录数量过多' }
    if (offset + 2 > message.length) return { records, error: 'NDEF 头不完整' }
    const header = message[offset++]
    const tnf = header & 0x07
    const short = (header & 0x10) !== 0
    const hasId = (header & 0x08) !== 0
    const chunked = (header & 0x20) !== 0
    const typeLen = message[offset++]
    if (offset + (short ? 1 : 4) > message.length) return { records, error: 'NDEF 长度字段不完整' }
    let payloadLen: number
    if (short) {
      payloadLen = message[offset++]
    } else {
      payloadLen = (message[offset] << 24) | (message[offset + 1] << 16) | (message[offset + 2] << 8) | message[offset + 3]
      offset += 4
    }
    const idLen = hasId ? message[offset++] : 0
    if (offset + typeLen + idLen + payloadLen > message.length) return { records, error: 'NDEF 负载超出消息长度' }
    const type = message.slice(offset, offset + typeLen)
    offset += typeLen
    const id = message.slice(offset, offset + idLen)
    offset += idLen
    const payload = message.slice(offset, offset + payloadLen)
    offset += payloadLen
    records.push(parseRecord(tnf, type, payload))
    if (chunked) {
      // 简化处理: 分块记录按独立记录展示, 后续块继续解析
      continue
    }
    if (header & 0x40) break
  }
  return { records }
}

export function parseCardNdef(card: Pick<RawCard, 'data' | 'formatted'>): NdefParse {
  if (!card.formatted) {
    return { ok: false, formatted: false, records: [], messageHex: '', tlvLength: 0, error: '标签尚未格式化为 NDEF' }
  }
  let data: Uint8Array
  try {
    data = hexToBytes(card.data || '')
  } catch (err) {
    return { ok: false, formatted: true, records: [], messageHex: '', tlvLength: 0, error: String(err) }
  }
  const tlv = extractNdefMessage(data)
  if (!tlv.ok) {
    return {
      ok: false,
      formatted: tlv.formatted,
      records: [],
      messageHex: bytesToHex(tlv.message),
      tlvLength: tlv.tlvLength,
      error: tlv.error
    }
  }
  const parsed = parseNdefMessage(tlv.message)
  return {
    ok: !parsed.error,
    formatted: true,
    records: parsed.records,
    messageHex: bytesToHex(tlv.message),
    tlvLength: tlv.tlvLength,
    error: parsed.error
  }
}

/** 生成包含 NDEF TLV 的 Type 2 用户区数据 (供模拟设备/预览使用) */
export function buildType2UserData(message: Uint8Array, capacity = 144): Uint8Array {
  const header: number[] = message.length < 255
    ? [0x03, message.length]
    : [0x03, 0xff, (message.length >> 8) & 0xff, message.length & 0xff]
  const total = header.length + message.length + 1
  if (total > capacity) throw new Error(`NDEF 内容 ${total}B 超过标签容量 ${capacity}B`)
  const out = new Uint8Array(capacity)
  out.set(header, 0)
  out.set(message, header.length)
  out[header.length + message.length] = 0xfe
  return out
}

export function describeNdef(parse: NdefParse): string {
  if (!parse.ok) return parse.error ?? '没有可显示的 NDEF 内容'
  if (parse.records.length === 0) return '空 NDEF 消息'
  const first = parse.records[0]
  return first.kind === 'text' || first.kind === 'uri' ? first.value : `${first.typeLabel} · ${first.value.slice(0, 48)}`
}

export function estimateTlvSize(messageLength: number): number {
  const header = messageLength < 255 ? 2 : 4
  return header + messageLength + 1
}
