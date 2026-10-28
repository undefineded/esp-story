import type { NfcCard, RawCard, TagKind } from '@shared/types'
import { parseCardNdef } from './ndef'

export function toNfcCard(raw: RawCard, previous?: NfcCard): NfcCard {
  const now = Date.now()
  return {
    ...raw,
    firstSeen: previous && previous.uid === raw.uid ? previous.firstSeen : now,
    lastSeen: now,
    ndef: parseCardNdef(raw)
  }
}

export function formatUid(uid: string): string {
  const clean = uid.replace(/[^0-9a-fA-F]/g, '').toUpperCase()
  return clean.match(/.{1,2}/g)?.join(':') ?? clean
}

export function shortUid(uid: string): string {
  const clean = uid.replace(/[^0-9a-fA-F]/g, '').toUpperCase()
  if (clean.length <= 8) return formatUid(clean)
  return `${formatUid(clean.slice(-8))}`
}

export function kindGroup(kind: TagKind): 'type2' | 'classic' | 'iso4' | 'unknown' {
  if (kind.startsWith('ntag') || kind === 'ultralight') return 'type2'
  if (kind.startsWith('mifare-classic') || kind === 'mifare-mini') return 'classic'
  if (kind === 'iso14443a-4') return 'iso4'
  return 'unknown'
}

export function kindAccent(kind: TagKind): string {
  switch (kindGroup(kind)) {
    case 'type2': return 'aqua'
    case 'classic': return 'amber'
    case 'iso4': return 'violet'
    default: return 'slate'
  }
}

export function kindShort(kind: TagKind): string {
  switch (kind) {
    case 'ntag210': return 'NTAG210'
    case 'ntag212': return 'NTAG212'
    case 'ntag213': return 'NTAG213'
    case 'ntag215': return 'NTAG215'
    case 'ntag216': return 'NTAG216'
    case 'ultralight': return 'Ultralight'
    case 'mifare-classic-1k': return 'Classic 1K'
    case 'mifare-classic-4k': return 'Classic 4K'
    case 'mifare-mini': return 'MIFARE Mini'
    case 'iso14443a-4': return 'ISO 14443-4'
    default: return '未知卡片'
  }
}

export function capacityLabel(card: Pick<NfcCard, 'type2' | 'cap' | 'userTotal'>): string {
  const bytes = card.type2 ? card.cap || card.userTotal : card.cap
  if (!bytes) return '—'
  return `${bytes} B`
}

export function relativeTime(ts: number): string {
  const diff = Math.max(0, Date.now() - ts)
  if (diff < 8_000) return '刚刚'
  if (diff < 60_000) return `${Math.floor(diff / 1000)} 秒前`
  if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} 分钟前`
  if (diff < 86_400_000) return `${Math.floor(diff / 3_600_000)} 小时前`
  return new Date(ts).toLocaleDateString('zh-CN', { month: '2-digit', day: '2-digit' })
}

export function clockTime(ts: number): string {
  return new Date(ts).toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}
