import { useEffect, useMemo, useState } from 'react'
import { Download, LibraryBig, Search, Trash2, X } from 'lucide-react'
import type { NfcCard } from '@shared/types'
import { PageHeader } from '../components/PageHeader'
import { CardVisual } from '../components/CardVisual'
import { NdefRecords } from '../components/NdefRecords'
import { Button, CopyButton, EmptyState, Pill } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { capacityLabel, formatUid, kindAccent, kindGroup, relativeTime } from '../lib/card'
import { describeNdef } from '../lib/ndef'

type Filter = 'all' | 'type2' | 'classic'

export function LibraryPage() {
  const history = useAppStore((s) => s.history)
  const clearHistory = useAppStore((s) => s.clearHistory)
  const setView = useAppStore((s) => s.setView)
  const [query, setQuery] = useState('')
  const [filter, setFilter] = useState<Filter>('all')
  const [selected, setSelected] = useState<NfcCard | undefined>(undefined)

  const filtered = useMemo(() => {
    const needle = query.trim().toLowerCase()
    return history.filter((card) => {
      if (filter === 'type2' && kindGroup(card.kind) !== 'type2') return false
      if (filter === 'classic' && kindGroup(card.kind) !== 'classic') return false
      if (!needle) return true
      const haystack = [
        card.uid,
        card.type,
        card.ndef.records.map((r) => r.value).join(' '),
        card.ndef.error ?? ''
      ]
        .join(' ')
        .toLowerCase()
      return haystack.includes(needle)
    })
  }, [history, query, filter])

  useEffect(() => {
    function onKey(event: KeyboardEvent): void {
      if (event.key === 'Escape') setSelected(undefined)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  function exportAll(): void {
    const payload = {
      exportedAt: new Date().toISOString(),
      count: history.length,
      cards: history
    }
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = `nfc-library-${new Date().toISOString().slice(0, 10)}.json`
    anchor.click()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
  }

  return (
    <div className="page">
      <PageHeader
        view="library"
        actions={
          <>
            <Button variant="secondary" icon={<Download size={14} />} onClick={exportAll} disabled={history.length === 0}>
              导出全部
            </Button>
            <Button
              variant="secondary"
              icon={<Trash2 size={14} />}
              onClick={() => {
                if (history.length === 0) return
                if (window.confirm('确定清空本机保存的卡片快照吗？此操作不会影响卡片内容。')) clearHistory()
              }}
              disabled={history.length === 0}
            >
              清空
            </Button>
          </>
        }
      />

      <div className="library-toolbar">
        <div className="search">
          <Search size={15} className="search-icon" />
          <input
            className="input"
            placeholder="搜索 UID、类型或卡片内容…"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
          />
        </div>
        <div className="segmented">
          <button type="button" className={filter === 'all' ? 'active' : ''} onClick={() => setFilter('all')}>
            全部 · {history.length}
          </button>
          <button type="button" className={filter === 'type2' ? 'active' : ''} onClick={() => setFilter('type2')}>
            Type 2
          </button>
          <button type="button" className={filter === 'classic' ? 'active' : ''} onClick={() => setFilter('classic')}>
            Classic
          </button>
        </div>
        <span style={{ flex: 1 }} />
        <span className="tiny faint">
          {filtered.length} / {history.length} 张
        </span>
      </div>

      {filtered.length === 0 ? (
        <div className="panel">
          <EmptyState
            icon={<LibraryBig size={26} />}
            title={history.length === 0 ? '卡片库还是空的' : '没有匹配的卡片'}
            description={
              history.length === 0
                ? '读取过的卡片会自动保存快照；写入后内容也会同步更新。'
                : '试试更短的关键词，或切换筛选条件。'
            }
            actions={
              history.length === 0 ? (
                <Button variant="primary" size="sm" onClick={() => setView('reader')}>
                  去读一张卡
                </Button>
              ) : (
                <Button variant="secondary" size="sm" onClick={() => setQuery('')}>
                  清除搜索
                </Button>
              )
            }
          />
        </div>
      ) : (
        <div className="history-grid">
          {filtered.map((card) => (
            <button type="button" className="history-card" key={card.uid} onClick={() => setSelected(card)}>
              <div className="history-card-top">
                <span className={`history-swatch ${kindAccent(card.kind)}`}>
                  <LibraryBig size={18} />
                </span>
                <div className="history-copy">
                  <div className="history-type">{card.type}</div>
                  <div className="history-uid">{formatUid(card.uid)}</div>
                </div>
                <Pill tone={card.writable ? 'aqua' : 'red'}>{card.writable ? '可写' : '只读'}</Pill>
              </div>
              <div className="history-content">
                {card.ndef.ok
                  ? card.ndef.records.map((r) => r.value || r.typeLabel).join(' · ') || '空 NDEF 消息'
                  : card.ndef.error ?? describeNdef(card.ndef)}
              </div>
              <div className="history-foot">
                <span>{relativeTime(card.lastSeen)}</span>
                <span>·</span>
                <span>{capacityLabel(card)}</span>
                <span className="spacer" />
                <span>{card.ndef.records.length} 条记录</span>
              </div>
            </button>
          ))}
        </div>
      )}

      {selected && (
        <div className="modal-backdrop" onClick={() => setSelected(undefined)}>
          <div className="modal" onClick={(event) => event.stopPropagation()}>
            <div className="modal-head">
              <div style={{ flex: 1 }}>
                <div className="page-eyebrow" style={{ marginBottom: 6 }}>
                  CARD SNAPSHOT
                </div>
                <div className="panel-title">{selected.type}</div>
                <div className="mono small faint" style={{ marginTop: 3 }}>
                  {formatUid(selected.uid)}
                </div>
              </div>
              <button type="button" className="modal-close" onClick={() => setSelected(undefined)} aria-label="关闭">
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div className="library-detail">
                <div className="row wrap gap-16" style={{ alignItems: 'flex-start' }}>
                  <CardVisual card={selected} size="sm" />
                  <div className="flex-1" style={{ minWidth: 220 }}>
                    <div className="detail-list">
                      <div className="detail-row">
                        <span className="detail-key">UID</span>
                        <span className="detail-val mono">
                          {formatUid(selected.uid)} <CopyButton value={formatUid(selected.uid)} label="复制" />
                        </span>
                      </div>
                      <div className="detail-row">
                        <span className="detail-key">类型</span>
                        <span className="detail-val">{selected.type}</span>
                      </div>
                      <div className="detail-row">
                        <span className="detail-key">容量</span>
                        <span className="detail-val">{capacityLabel(selected)}</span>
                      </div>
                      <div className="detail-row">
                        <span className="detail-key">最近读取</span>
                        <span className="detail-val">
                          {new Date(selected.lastSeen).toLocaleString('zh-CN', { hour12: false })}
                        </span>
                      </div>
                    </div>
                  </div>
                </div>

                <div>
                  <div className="panel-title" style={{ marginBottom: 10 }}>NDEF 内容</div>
                  <NdefRecords parse={selected.ndef} compact />
                </div>
              </div>
            </div>
            <div className="modal-actions">
              <CopyButton value={JSON.stringify(selected, null, 2)} label="复制快照 JSON" />
              <Button variant="secondary" size="sm" onClick={() => setView('writer')}>
                去写卡
              </Button>
              <Button variant="primary" size="sm" onClick={() => setSelected(undefined)}>
                完成
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
