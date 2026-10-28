import type { NfcCard, RawCard } from '@shared/types'
import { toNfcCard } from './card'
import { buildNdefMessage, buildType2UserData } from './ndef'

function ntag213(uid: string, text: string): RawCard {
  const message = buildNdefMessage([{ kind: 'text', text, language: 'zh' }])
  const data = buildType2UserData(message, 144)
  return {
    uid,
    uidLen: 7,
    atqa: '0044',
    sak: '00',
    kind: 'ntag213',
    type: 'NTAG213',
    std: 'ISO 14443A · Type 2',
    type2: true,
    writable: true,
    formatted: true,
    cap: 144,
    userStart: 4,
    userPages: 36,
    userTotal: 144,
    userBytes: 144,
    version: '0004040201000F03',
    cc: 'E1101200',
    lock: '0000',
    data: Array.from(data, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
  }
}

function ntag215(uid: string, uri: string): RawCard {
  const message = buildNdefMessage([{ kind: 'uri', uri }])
  const data = buildType2UserData(message, 504)
  return {
    uid,
    uidLen: 7,
    atqa: '0044',
    sak: '00',
    kind: 'ntag215',
    type: 'NTAG215',
    std: 'ISO 14443A · Type 2',
    type2: true,
    writable: true,
    formatted: true,
    cap: 504,
    userStart: 4,
    userPages: 126,
    userTotal: 504,
    userBytes: 256,
    version: '0004040201001103',
    cc: 'E1103E00',
    lock: '0000',
    data: Array.from(data.slice(0, 256), (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
  }
}

function classic1k(uid: string, title: string): RawCard {
  const block0 = `A1B2C3D4E5F6${uid.slice(-8)}0000000000000000`.slice(0, 32).toUpperCase()
  return {
    uid,
    uidLen: 4,
    atqa: '0004',
    sak: '08',
    kind: 'mifare-classic-1k',
    type: 'MIFARE Classic 1K',
    std: 'ISO 14443A · MIFARE Classic',
    type2: false,
    writable: true,
    formatted: false,
    cap: 1024,
    userStart: 0,
    userPages: 64,
    userTotal: 1024,
    userBytes: 16,
    lock: '0000',
    data: block0,
    block0
  }
}

const HISTORY_OFFSETS = [4 * 60_000, 42 * 60_000, 3 * 3_600_000, 26 * 3_600_000]

export function demoHistoryCards(): NfcCard[] {
  const raws = [
    ntag213('04A26B9E3C5180', '月亮邮递员 · 第 03 集'),
    ntag215('04C81D4472AB90', 'https://storybox.local/album/nursery'),
    classic1k('9A7F21C4', '送给你的一首小诗'),
    ntag213('04E19F03A8B271', '小星星 · 童谣合辑')
  ]
  return raws.map((raw, index) => {
    const card = toNfcCard(raw)
    card.firstSeen = Date.now() - HISTORY_OFFSETS[index] - 20_000
    card.lastSeen = Date.now() - HISTORY_OFFSETS[index]
    return card
  })
}

export function demoLiveCard(): RawCard {
  return ntag213('04A26B9E3C5180', '月亮邮递员 · 第 03 集')
}

export function demoCardByUid(uid: string): RawCard | undefined {
  return demoHistoryCards().find((card) => card.uid === uid)
}
