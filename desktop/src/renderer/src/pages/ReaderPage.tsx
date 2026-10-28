import { useEffect, useMemo, useState } from 'react'
import {
  Braces,
  Download,
  History,
  Nfc,
  Pause,
  Play,
  RefreshCw,
  ScanLine,
  ShieldCheck,
  Sparkles
} from 'lucide-react'
import { PageHeader } from '../components/PageHeader'
import { NfcScanner } from '../components/NfcScanner'
import { NdefRecords } from '../components/NdefRecords'
import { HexDump } from '../components/HexDump'
import { Button, CopyButton, EmptyState, InfoBanner, Pill } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { capacityLabel, formatUid, kindAccent, kindGroup, relativeTime } from '../lib/card'

type Tab = 'overview' | 'ndef' | 'raw'

interface RawData {
  hex: string
  start: number
  kind: 'page' | 'block'
}

export function ReaderPage() {
  const card = useAppStore((s) => s.card)
  const nfc = useAppStore((s) => s.nfc)
  const connection = useAppStore((s) => s.connection)
  const demo = useAppStore((s) => s.demo)
  const setView = useAppStore((s) => s.setView)
  const setPolling = useAppStore((s) => s.setPolling)
  const refresh = useAppStore((s) => s.refresh)
  const readRaw = useAppStore((s) => s.readRaw)
  const demoPlace = useAppStore((s) => s.demoPlaceCard)
  const demoRemove = useAppStore((s) => s.demoRemoveCard)
  const [tab, setTab] = useState<Tab>('overview')
  const [raw, setRaw] = useState<RawData | undefined>(undefined)
  const [rawLoading, setRawLoading] = useState(false)

  const connected = connection.state === 'connected'
  const type2 = card?.type2 ?? true

  useEffect(() => {
    setRaw(undefined)
    if (tab === 'raw' && card) void loadRaw(card.type2 ? 'page' : 'block', 0, card.type2 ? 16 : 4)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [card?.uid, tab])

  async function loadRaw(kind: 'page' | 'block', start: number, count: number): Promise<void> {
    setRawLoading(true)
    const result = await readRaw({ kind, start, count })
    setRawLoading(false)
    if (result) {
      setRaw({ hex: result.hex, start: result.start, kind })
    }
  }

  const userBytesRead = useMemo(() => (raw ? raw.hex.length / 2 : 0), [raw])

  function exportJson(): void {
    if (!card) return
    const payload = {
      exportedAt: new Date().toISOString(),
      device: connection.device ? { name: connection.device.name, fw: connection.device.fw, board: connection.device.board } : null,
      card,
      raw: raw
        ? {
            kind: raw.kind,
            start: raw.start,
            hex: raw.hex
          }
        : null
    }
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = `nfc-card-${card.uid}.json`
    anchor.click()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
  }

  return (
    <div className="page">
      <PageHeader
        view="reader"
        actions={
          <>
            <Button
              variant="secondary"
              icon={nfc.polling ? <Pause size={14} /> : <Play size={14} />}
              onClick={() => void setPolling(!nfc.polling)}
              disabled={!connected}
            >
              {nfc.polling ? '暂停轮询' : '开始轮询'}
            </Button>
            <Button
              variant="secondary"
              icon={<RefreshCw size={14} />}
              onClick={() => void refresh()}
              disabled={!connected}
            >
              刷新
            </Button>
            <Button variant="primary" icon={<Download size={14} />} onClick={exportJson} disabled={!card}>
              导出 JSON
            </Button>
          </>
        }
      />

      {connected && !nfc.present && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="warn" icon={<Nfc size={16} />}>
            读卡模块没有响应。检查 PN532 的 I2C 接线和拨码，或在设备页重新检测。
          </InfoBanner>
        </div>
      )}

      <div className="reader-grid">
        <div className="reader-side">
          <NfcScanner className="reader-scanner" compact showActions={false} />

          <div className="panel reader-card-panel">
            <div className="row gap-10">
              <div className="flex-1">
                <div className="panel-title">{card ? card.type : '等待卡片'}</div>
                <div className="panel-sub">
                  {card ? `${kindGroup(card.kind) === 'type2' ? 'Type 2 标签' : kindGroup(card.kind) === 'classic' ? 'MIFARE Classic' : 'ISO 14443A'}` : '靠近 PN532 天线即可读取'}
                </div>
              </div>
              {card && <Pill tone={kindAccent(card.kind) as 'aqua'} dot>{card.writable ? '可写' : '只读'}</Pill>}
            </div>

            {card ? (
              <>
                <div className="row gap-6" style={{ marginTop: 14 }}>
                  <span className="mono" style={{ fontSize: 12, letterSpacing: '0.06em' }}>
                    {formatUid(card.uid)}
                  </span>
                  <CopyButton value={formatUid(card.uid)} label="复制 UID" />
                </div>
                <div className="reader-card-meta">
                  <div className="mini-meta">
                    <div className="mini-meta-label">容量</div>
                    <div className="mini-meta-value">{capacityLabel(card)}</div>
                  </div>
                  <div className="mini-meta">
                    <div className="mini-meta-label">NDEF</div>
                    <div className="mini-meta-value">{card.ndef.records.length} 条</div>
                  </div>
                  <div className="mini-meta">
                    <div className="mini-meta-label">ATQA / SAK</div>
                    <div className="mini-meta-value">
                      {card.atqa} / {card.sak}
                    </div>
                  </div>
                  <div className="mini-meta">
                    <div className="mini-meta-label">最近</div>
                    <div className="mini-meta-value">{relativeTime(card.lastSeen)}</div>
                  </div>
                </div>
                <div className="row gap-8" style={{ marginTop: 14 }}>
                  <Button variant="primary" size="sm" onClick={() => setView('writer')} icon={<Sparkles size={13} />}>
                    写卡
                  </Button>
                  {demo && (
                    <Button variant="secondary" size="sm" onClick={demoRemove} icon={<Nfc size={13} />}>
                      移开卡片
                    </Button>
                  )}
                </div>
              </>
            ) : (
              <div className="empty-mini" style={{ padding: '20px 6px 4px' }}>
                <span>天线范围内没有卡片</span>
                {demo && (
                  <Button variant="soft" size="sm" onClick={demoPlace}>
                    模拟放置卡片
                  </Button>
                )}
              </div>
            )}
          </div>

          {demo && !card && (
            <div className="panel" style={{ padding: 14 }}>
              <div className="row gap-8">
                <Sparkles size={15} style={{ color: 'var(--yellow-strong)' }} />
                <span className="small muted">演示模式会持续模拟一张 NTAG213，写入后立即回显。</span>
              </div>
            </div>
          )}
        </div>

        <div className="panel detail-panel">
          <div className="panel-head">
            <div className="panel-title-row">
              <div className="panel-title">卡片详情</div>
              <div className="panel-sub">UID、协议参数与存储区原始数据</div>
            </div>
            <div className="reader-actions">
              <CopyButton value={card ? JSON.stringify(card, null, 2) : ''} label="复制快照" />
            </div>
          </div>

          <div className="panel-body">
            <div className="tabs" style={{ marginBottom: 18 }}>
              <button
                type="button"
                className={`tab ${tab === 'overview' ? 'active' : ''}`}
                onClick={() => setTab('overview')}
              >
                概览
              </button>
              <button
                type="button"
                className={`tab ${tab === 'ndef' ? 'active' : ''}`}
                onClick={() => setTab('ndef')}
              >
                NDEF 记录{card ? ` · ${card.ndef.records.length}` : ''}
              </button>
              <button
                type="button"
                className={`tab ${tab === 'raw' ? 'active' : ''}`}
                onClick={() => setTab('raw')}
              >
                原始数据
              </button>
            </div>

            {!card ? (
              <EmptyState
                icon={<ScanLine size={26} />}
                title="请放置一张 NFC 卡片"
                description="把卡片放在 PN532 线圈上方 2–3 cm 内，读卡器会自动识别 UID 与 NDEF 内容。"
                actions={
                  <Button variant="secondary" size="sm" onClick={() => void refresh()} disabled={!connected}>
                    重新扫描
                  </Button>
                }
              />
            ) : tab === 'overview' ? (
              <div className="detail-list">
                <div className="detail-row">
                  <span className="detail-key">卡片类型</span>
                  <span className="detail-val">
                    {card.type} <Pill tone={kindAccent(card.kind) as 'aqua'} dot>{card.type2 ? 'Type 2' : 'Classic'}</Pill>
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">协议标准</span>
                  <span className="detail-val">{card.std}</span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">UID</span>
                  <span className="detail-val mono">{formatUid(card.uid)}</span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">ATQA / SAK</span>
                  <span className="detail-val mono">
                    {card.atqa} / {card.sak}
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">用户容量</span>
                  <span className="detail-val">
                    {capacityLabel(card)}
                    {card.type2 && <span className="faint"> · {card.userPages} 页</span>}
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">写入能力</span>
                  <span className="detail-val">
                    {card.writable ? (
                      <Pill tone="aqua" dot>可写</Pill>
                    ) : (
                      <Pill tone="red" dot>只读 / 已锁定</Pill>
                    )}
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">NDEF 状态</span>
                  <span className="detail-val">
                    {card.formatted ? '已格式化' : '未格式化'}
                    {card.formatted && card.cc && <span className="mono faint"> · CC {card.cc}</span>}
                  </span>
                </div>
                {card.type2 && (
                  <div className="detail-row">
                    <span className="detail-key">版本 / 锁定位</span>
                    <span className="detail-val mono">
                      {card.version || '—'} <span className="faint">/ {card.lock || '0000'}</span>
                    </span>
                  </div>
                )}
                {!card.type2 && (
                  <div className="detail-row">
                    <span className="detail-key">Block 0</span>
                    <span className="detail-val mono">{card.block0 || '—'}</span>
                  </div>
                )}
                <div className="detail-row">
                  <span className="detail-key">最近读取</span>
                  <span className="detail-val">
                    {new Date(card.lastSeen).toLocaleString('zh-CN', { hour12: false })}
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-key">数据摘要</span>
                  <span className="detail-val small">
                    {card.ndef.ok ? card.ndef.records.map((r) => `${r.typeLabel}: ${r.value}`).join(' · ') : card.ndef.error}
                  </span>
                </div>
                <div className="info-banner" style={{ marginTop: 16 }}>
                  <ShieldCheck size={16} />
                  <span>写入采用 NDEF 标准格式，写入完成后固件会回读比对；不会修改锁定位与配置页。</span>
                </div>
              </div>
            ) : tab === 'ndef' ? (
              <NdefRecords parse={card.ndef} onWrite={() => setView('writer')} />
            ) : (
              <div>
                <div className="hex-toolbar">
                  <Button
                    variant="secondary"
                    size="sm"
                    loading={rawLoading}
                    onClick={() =>
                      void loadRaw(card.type2 ? 'page' : 'block', 0, card.type2 ? 16 : 4)
                    }
                    icon={<Braces size={13} />}
                  >
                    读取头部
                  </Button>
                  <Button
                    variant="secondary"
                    size="sm"
                    loading={rawLoading}
                    onClick={() =>
                      void loadRaw(
                        card.type2 ? 'page' : 'block',
                        card.type2 ? 4 : 0,
                        card.type2 ? Math.min(64, card.userPages) : Math.min(16, Math.floor(card.cap / 16))
                      )
                    }
                    icon={<History size={13} />}
                  >
                    {card.type2 ? '读取用户区' : '读取 16 块'}
                  </Button>
                  {raw && (
                    <>
                      <Pill tone="muted">
                        {raw.kind === 'page' ? `Page ${raw.start}` : `Block ${raw.start}`} · {userBytesRead} B
                      </Pill>
                      <CopyButton value={raw.hex} label="复制 HEX" />
                    </>
                  )}
                  <span style={{ flex: 1 }} />
                  {!type2 && <Pill tone="amber">使用默认密钥 FFFFFFFFFFFF</Pill>}
                </div>
                {raw ? (
                  <HexDump hex={raw.hex} start={raw.start} unit={raw.kind} />
                ) : (
                  <EmptyState
                    icon={<Braces size={24} />}
                    title="还没有读取原始数据"
                    description={
                      card.type2
                        ? '读取头部会返回 Page 0–15，包含 UID、锁定位、CC 和用户区开头。'
                        : 'Classic 卡片需要先认证扇区，默认使用全 F 密钥。'
                    }
                    actions={
                      <Button
                        variant="secondary"
                        size="sm"
                        loading={rawLoading}
                        onClick={() => void loadRaw(card.type2 ? 'page' : 'block', 0, card.type2 ? 16 : 4)}
                      >
                        立即读取
                      </Button>
                    }
                  />
                )}
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
