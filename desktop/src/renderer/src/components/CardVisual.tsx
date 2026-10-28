import { Wifi } from 'lucide-react'
import type { RawCard } from '@shared/types'
import { formatUid, kindAccent, kindShort } from '../lib/card'

export function CardVisual({
  card,
  size = 'md',
  className = '',
  emptyLabel = '等待卡片'
}: {
  card?: Pick<RawCard, 'uid' | 'kind' | 'type'> | null
  size?: 'sm' | 'md'
  className?: string
  emptyLabel?: string
}) {
  if (!card) {
    return (
      <div className={`nfc-card nfc-card-empty ${size === 'sm' ? 'nfc-card-sm' : ''} ${className}`.trim()}>
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
          <Wifi size={26} style={{ opacity: 0.6, transform: 'rotate(90deg)' }} />
          <span style={{ fontSize: 11, letterSpacing: '0.12em', fontWeight: 700 }}>{emptyLabel}</span>
        </div>
      </div>
    )
  }

  return (
    <div className={`nfc-card ${kindAccent(card.kind)} ${size === 'sm' ? 'nfc-card-sm' : ''} ${className}`.trim()}>
      <div className="nfc-card-top">
        <div>
          <div className="nfc-card-kicker">StoryBox · NFC</div>
          <div className="nfc-card-brand">小小故事盒</div>
        </div>
        <div className="nfc-card-mark" title="NFC">
          <Wifi size={size === 'sm' ? 16 : 20} style={{ transform: 'rotate(90deg)' }} />
        </div>
      </div>
      <div className="nfc-card-chip" />
      <div className="nfc-card-foot">
        <div>
          <div className="nfc-card-type">{kindShort(card.kind)}</div>
          <div className="nfc-card-uid">{formatUid(card.uid)}</div>
        </div>
      </div>
    </div>
  )
}
