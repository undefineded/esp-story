import { useEffect, useMemo, useRef, useState } from 'react'
import {
  ArrowRight,
  BadgeCheck,
  BookOpen,
  Braces,
  Check,
  ChevronDown,
  ChevronRight,
  DownloadCloud,
  Folder,
  FolderOpen,
  Music2,
  Nfc,
  PenLine,
  RefreshCw,
  Search,
  ShieldAlert,
  Sparkles
} from 'lucide-react'
import type { NdefParse, NdefRecord } from '@shared/types'
import { PageHeader } from '../components/PageHeader'
import { CardVisual } from '../components/CardVisual'
import { Button, InfoBanner, Pill, Toggle } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { capacityLabel, kindAccent } from '../lib/card'
import { buildNdefMessage, bytesToHex, estimateTlvSize, parseNdefMessage } from '../lib/ndef'
import {
  buildResourceUri,
  parseResourceUri,
  resourceFolderLabel,
  type StoryboxResource,
  type ResourceKind
} from '../lib/resources'

function formatSize(bytes: number): string {
  if (!bytes) return '—'
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
  if (bytes >= 1024) return `${Math.round(bytes / 1024)} KB`
  return `${bytes} B`
}

function kindLabel(kind: ResourceKind): string {
  return kind === 'music' ? '音乐' : '故事'
}

interface ResourceFolderGroup {
  key: string
  label: string
  items: StoryboxResource[]
}

interface ResourceCategoryGroup {
  key: string
  kind: ResourceKind
  category: string
  folders: ResourceFolderGroup[]
}

export function WriterPage() {
  const card = useAppStore((s) => s.card)
  const write = useAppStore((s) => s.write)
  const demo = useAppStore((s) => s.demo)
  const setView = useAppStore((s) => s.setView)
  const showToast = useAppStore((s) => s.showToast)
  const refresh = useAppStore((s) => s.refresh)
  const demoPlace = useAppStore((s) => s.demoPlaceCard)
  const connection = useAppStore((s) => s.connection)
  const resources = useAppStore((s) => s.resources)
  const resourcesSyncedAt = useAppStore((s) => s.resourcesSyncedAt)
  const resourcesSyncing = useAppStore((s) => s.resourcesSyncing)
  const resourcesError = useAppStore((s) => s.resourcesError)
  const syncResources = useAppStore((s) => s.syncResources)
  const writeResource = useAppStore((s) => s.writeResource)

  const [selectedId, setSelectedId] = useState<string>()
  const [query, setQuery] = useState('')
  const [verify, setVerify] = useState(true)
  const [collapsed, setCollapsed] = useState<Set<string>>(new Set())
  const autoSyncAttempted = useRef(false)

  const connected = connection.state === 'connected'

  useEffect(() => {
    if (!connected) {
      autoSyncAttempted.current = false
      return
    }
    // Auto-sync once per connection when there is no cache. An empty result must
    // not retrigger this effect, or a slow/empty card would rescan in a loop.
    if (autoSyncAttempted.current) return
    if (resources.length === 0 && !resourcesSyncing && !resourcesSyncedAt) {
      autoSyncAttempted.current = true
      void syncResources()
    }
  }, [connected, resources.length, resourcesSyncing, resourcesSyncedAt, syncResources])

  useEffect(() => {
    if (selectedId && !resources.some((item) => item.id === selectedId)) setSelectedId(undefined)
  }, [resources, selectedId])

  // 放上的卡片如果已经是一张资源卡，自动选中对应资源（不覆盖用户当前选择）。
  useEffect(() => {
    if (selectedId || !card || resources.length === 0) return
    const uri = card.ndef.records.find((record) => record.kind === 'uri')?.uri
    const path = uri ? parseResourceUri(uri) : null
    const match = path ? resources.find((item) => item.path === path) : undefined
    if (match) setSelectedId(match.id)
  }, [card?.ndef.messageHex, resources, selectedId])

  const selected = useMemo(
    () => resources.find((item) => item.id === selectedId),
    [resources, selectedId]
  )

  const filtered = useMemo(() => {
    const keyword = query.trim().toLowerCase()
    if (!keyword) return resources
    return resources.filter(
      (item) =>
        item.title.toLowerCase().includes(keyword) ||
        item.category.toLowerCase().includes(keyword) ||
        item.path.toLowerCase().includes(keyword)
    )
  }, [resources, query])

  const groups = useMemo<ResourceCategoryGroup[]>(() => {
    const categories = new Map<string, { group: ResourceCategoryGroup; folders: Map<string, ResourceFolderGroup> }>()
    for (const item of filtered) {
      const categoryKey = `${item.kind}:${item.category}`
      let category = categories.get(categoryKey)
      if (!category) {
        category = {
          group: { key: categoryKey, kind: item.kind, category: item.category, folders: [] },
          folders: new Map()
        }
        categories.set(categoryKey, category)
      }
      const label = resourceFolderLabel(item)
      const folderKey = `${categoryKey}:${item.directory || label}`
      let folder = category.folders.get(folderKey)
      if (!folder) {
        folder = { key: folderKey, label, items: [] }
        category.folders.set(folderKey, folder)
      }
      folder.items.push(item)
    }

    const result = [...categories.values()].map(({ group, folders }) => ({
      ...group,
      folders: [...folders.values()].sort((a, b) => {
        if (a.label === '直属文件' || a.label === '本目录') return -1
        if (b.label === '直属文件' || b.label === '本目录') return 1
        return a.label.localeCompare(b.label, 'zh-CN')
      })
    }))
    return result.sort((a, b) => {
      if (a.kind !== b.kind) return a.kind === 'music' ? -1 : 1
      return a.category.localeCompare(b.category, 'zh-CN')
    })
  }, [filtered])

  const forceOpen = query.trim().length > 0

  function toggleGroup(key: string): void {
    setCollapsed((previous) => {
      const next = new Set(previous)
      if (next.has(key)) next.delete(key)
      else next.add(key)
      return next
    })
  }

  const message = useMemo<Uint8Array | null>(() => {
    if (!selected) return null
    try {
      return buildNdefMessage([{ kind: 'uri', uri: buildResourceUri(selected.path) }])
    } catch {
      return null
    }
  }, [selected])

  const parse: NdefParse | null = useMemo(() => {
    if (!message) return null
    const parsed = parseNdefMessage(message)
    return {
      ok: !parsed.error,
      formatted: true,
      records: parsed.records,
      messageHex: bytesToHex(message),
      tlvLength: estimateTlvSize(message.length),
      error: parsed.error
    }
  }, [message])

  const needed = message ? estimateTlvSize(message.length) : 0
  const capacity = card?.type2 ? card.cap : 0
  const over = capacity > 0 && needed > capacity
  const writing = ['start', 'writing', 'verifying'].includes(write.phase)
  const writeDone = write.phase === 'done'
  const writeError = write.phase === 'error'
  const canWrite =
    Boolean(card) &&
    card?.type2 === true &&
    card?.writable === true &&
    Boolean(selected) &&
    Boolean(message) &&
    !over &&
    !writing

  const syncedLabel = resourcesSyncedAt
    ? new Date(resourcesSyncedAt).toLocaleString('zh-CN', {
        month: '2-digit',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit'
      })
    : '尚未同步'

  function readCardResource(): void {
    if (!card?.ndef.ok) {
      showToast('info', '卡片没有可读的 NDEF', '请先放一张已写入的资源卡')
      return
    }
    const uri = card.ndef.records.find((record) => record.kind === 'uri')?.uri
    const path = uri ? parseResourceUri(uri) : null
    if (!path) {
      showToast('info', '这张卡不是故事机资源卡', '当前写入流程只支持 TF 卡资源卡片')
      return
    }
    const match = resources.find((item) => item.path === path)
    if (!match) {
      showToast('info', '资源不在当前内容库', `卡片指向 ${path}，请先同步 TF 卡内容`)
      return
    }
    setSelectedId(match.id)
    showToast('success', '已选中卡片资源', match.title)
  }

  async function submit(): Promise<void> {
    if (selected) await writeResource(selected, verify)
  }

  return (
    <div className="page">
      <PageHeader
        view="writer"
        actions={
          <>
            <Button
              variant="secondary"
              icon={<DownloadCloud size={14} />}
              onClick={() => void syncResources()}
              disabled={!connected || resourcesSyncing}
              loading={resourcesSyncing}
            >
              同步内容库
            </Button>
            <Button
              variant="secondary"
              icon={<BookOpen size={14} />}
              onClick={readCardResource}
              disabled={!card?.ndef.ok}
            >
              读取卡片资源
            </Button>
            <Button variant="secondary" icon={<RefreshCw size={14} />} onClick={() => void refresh()} disabled={!card}>
              刷新卡片
            </Button>
          </>
        }
      />

      {!connected && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="info" icon={<DownloadCloud size={16} />}>
            设备未连接，当前显示的是上次同步的内容库缓存。连接故事机后点“同步内容库”可刷新 TF 卡资源。
          </InfoBanner>
        </div>
      )}

      {!card && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="warn" icon={<Nfc size={16} />}>
            写卡前需要先把卡片放在 PN532 天线上。
            {demo && (
              <button type="button" className="btn btn-soft btn-sm" style={{ marginLeft: 10 }} onClick={demoPlace}>
                模拟放置卡片
              </button>
            )}
            {!demo && (
              <button
                type="button"
                className="btn btn-ghost btn-sm"
                style={{ marginLeft: 10 }}
                onClick={() => setView('reader')}
              >
                打开读卡器 <ArrowRight size={12} />
              </button>
            )}
          </InfoBanner>
        </div>
      )}

      {card && !card.type2 && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="warn" icon={<ShieldAlert size={16} />}>
            {card.type} 不支持标准 NDEF。资源卡片需要 NTAG / Type 2 标签。
          </InfoBanner>
        </div>
      )}

      <div className="writer-grid">
        <div className="writer-main">
          <div className="panel writer-form-panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">写入资源卡片</div>
                <div className="panel-sub">只写入 TF 卡上已有的音乐 / 故事，不能自定义文本或链接</div>
              </div>
              {card && (
                <Pill tone={kindAccent(card.kind) as 'aqua'} dot>
                  {card.type}
                </Pill>
              )}
            </div>

            <div className="panel-body">
              <section>
                <div className="form-section-title">
                  <span className="step-num">01</span>
                  <h3>选择故事机资源</h3>
                  <span className="form-section-sub">来源：设备 /music 与 /story</span>
                </div>

                <div className="resource-toolbar">
                  <div className="resource-search">
                    <Search size={14} />
                    <input
                      className="resource-search-input"
                      value={query}
                      onChange={(event) => setQuery(event.target.value)}
                      placeholder="搜索标题、分类或路径"
                    />
                  </div>
                  <Pill tone={resources.length > 0 ? 'aqua' : 'muted'}>{resources.length} 项</Pill>
                  <span className="sync-status">
                    {resourcesSyncing ? '正在同步…' : `上次同步 ${syncedLabel}`}
                  </span>
                </div>

                {resourcesError && (
                  <div style={{ marginBottom: 10 }}>
                    <InfoBanner tone="warn" icon={<ShieldAlert size={15} />}>
                      {resourcesError}
                    </InfoBanner>
                  </div>
                )}

                {resources.length === 0 ? (
                  <div className="resource-empty">
                    <DownloadCloud size={22} />
                    <span>
                      {connected
                        ? resourcesSyncing
                          ? '正在读取 TF 卡内容…'
                          : '还没有同步内容库'
                        : '连接故事机后同步 TF 卡内容'}
                    </span>
                    <Button
                      variant="secondary"
                      size="sm"
                      onClick={() => void syncResources()}
                      disabled={!connected || resourcesSyncing}
                      loading={resourcesSyncing}
                    >
                      同步内容库
                    </Button>
                  </div>
                ) : filtered.length === 0 ? (
                  <div className="resource-empty">
                    <Search size={20} />
                    <span>没有匹配“{query.trim()}”的资源</span>
                  </div>
                ) : (
                  <div className="resource-list">
                    {groups.map((group) => {
                      const open = forceOpen || !collapsed.has(group.key)
                      const count = group.folders.reduce((sum, folder) => sum + folder.items.length, 0)
                      return (
                        <div className="resource-group" key={group.key}>
                          <button
                            type="button"
                            className="resource-group-head"
                            onClick={() => toggleGroup(group.key)}
                            aria-expanded={open}
                          >
                            {group.kind === 'music' ? <Music2 size={12} /> : <BookOpen size={12} />}
                            <span>
                              {kindLabel(group.kind)} · {group.category}
                            </span>
                            <span className="resource-count">{count}</span>
                            {open ? <ChevronDown size={13} /> : <ChevronRight size={13} />}
                          </button>
                          {open &&
                            group.folders.map((folder) => {
                              const folderOpen = forceOpen || !collapsed.has(folder.key)
                              return (
                                <div className="resource-folder" key={folder.key}>
                                  <button
                                    type="button"
                                    className="resource-folder-head"
                                    onClick={() => toggleGroup(folder.key)}
                                    aria-expanded={folderOpen}
                                  >
                                    {folderOpen ? <FolderOpen size={12} /> : <Folder size={12} />}
                                    <span>{folder.label}</span>
                                    <span className="resource-count">{folder.items.length}</span>
                                    {folderOpen ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
                                  </button>
                                  {folderOpen &&
                                    folder.items.map((item) => (
                                      <button
                                        type="button"
                                        key={item.id}
                                        className={`resource-item ${item.id === selectedId ? 'selected' : ''}`}
                                        onClick={() => setSelectedId(item.id)}
                                      >
                                        <span className="resource-icon">
                                          {item.kind === 'music' ? <Music2 size={14} /> : <BookOpen size={14} />}
                                        </span>
                                        <span className="resource-text">
                                          <span className="resource-title">{item.title}</span>
                                          <span className="resource-meta">
                                            {item.illustrated
                                              ? '图文故事'
                                              : item.kind === 'music'
                                                ? '音乐'
                                                : '音频故事'}{' '}
                                            · {formatSize(item.size)} · {item.path}
                                          </span>
                                        </span>
                                        {item.id === selectedId && <Check size={15} />}
                                      </button>
                                    ))}
                                </div>
                              )
                            })}
                        </div>
                      )
                    })}
                  </div>
                )}

                {selected && (
                  <div className="selected-resource">
                    <div className="row gap-8">
                      <span className="tiny faint">写入资源</span>
                      <Pill tone="aqua">{kindLabel(selected.kind)}</Pill>
                    </div>
                    <div className="strong">{selected.title}</div>
                    <div className="mono tiny faint">{selected.path}</div>
                  </div>
                )}
              </section>

              <hr className="divider" />

              <section>
                <div className="form-section-title">
                  <span className="step-num">02</span>
                  <h3>写入选项</h3>
                  <span className="form-section-sub">卡片只记录资源路径，不复制媒体文件</span>
                </div>
                <div className="row wrap gap-16">
                  <Toggle
                    checked={verify}
                    onChange={setVerify}
                    title="写入后回读校验"
                    sub="逐页比较内容，失败会报告错误"
                  />
                  <span style={{ flex: 1 }} />
                  <Pill tone={card?.writable ? 'aqua' : 'red'} dot>
                    {card ? (card.writable ? '标签可写' : '标签只读') : '未放置卡片'}
                  </Pill>
                </div>
                <div className="field-hint" style={{ marginTop: 10 }}>
                  写入一条 NDEF URI：<span className="mono">storybox://media?path=…</span>。资源被删除或改名后，
                  这张卡会失效，需要重新同步并写卡。
                </div>
              </section>
            </div>

            <div className="write-actions">
              <div className="write-hint">
                {!selected
                  ? '先同步内容库并选择一个资源。'
                  : !card
                    ? '放置卡片后即可写入。'
                    : !card.type2
                      ? '当前卡片不支持 NDEF 资源写入。'
                      : !card.writable
                        ? '当前标签只读，无法写入。'
                        : over
                          ? '路径超过标签容量，请换 NTAG215/216 或缩短文件名。'
                          : `将写入 ${needed} B 资源卡片，剩余 ${Math.max(0, capacity - needed)} B。写入过程请保持卡片不动。`}
              </div>
              <Button
                variant="primary"
                size="lg"
                onClick={() => void submit()}
                disabled={!canWrite}
                loading={writing}
                icon={<PenLine size={16} />}
              >
                {writing ? '正在写入…' : writeDone ? '再写一次' : '写入卡片'}
              </Button>
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">记录预览</div>
                <div className="panel-sub">写入前会以这条 NDEF 消息发送给设备</div>
              </div>
              <Pill tone="muted">{message ? `${message.length} B` : '空'}</Pill>
            </div>
            <div className="panel-body">
              {!message ? (
                <div className="empty-mini">选择一个资源后这里会显示编码结果</div>
              ) : (
                <>
                  {selected && (
                    <div className="selected-resource" style={{ marginTop: 0, marginBottom: 12 }}>
                      <div className="row gap-8">
                        <span className="tiny faint">资源</span>
                        <Pill tone="aqua">{kindLabel(selected.kind)}</Pill>
                      </div>
                      <div className="strong">{selected.title}</div>
                      <div className="mono tiny faint">{buildResourceUri(selected.path)}</div>
                    </div>
                  )}
                  <div className="ndef-list">
                    {parse?.records.map((record) => (
                      <PreviewRecord key={record.id} record={record} />
                    ))}
                  </div>
                  <div className="code-block" style={{ marginTop: 12 }}>
                    {bytesToHex(message).match(/.{1,48}/g)?.join('\n') ?? ''}
                  </div>
                </>
              )}
            </div>
          </div>
        </div>

        <div className="writer-side">
          <div className="panel preview-card">
            <div className="preview-label">Target card</div>
            <CardVisual card={card} />
            {card ? (
              <div className="stack gap-6" style={{ width: '100%' }}>
                <div className="row gap-8">
                  <Pill tone={kindAccent(card.kind) as 'aqua'} dot>
                    {card.type}
                  </Pill>
                  <span style={{ flex: 1 }} />
                  <span className="tiny faint">{capacityLabel(card)}</span>
                </div>
                <div className="capacity-track" style={{ marginTop: 4 }}>
                  <div
                    className={`capacity-fill ${over ? 'over' : ''}`}
                    style={{
                      width: `${Math.min(100, card.cap ? (needed / card.cap) * 100 : 0)}%`
                    }}
                  />
                </div>
                <div className="capacity-foot">
                  <span>当前内容</span>
                  <span>{needed} B</span>
                </div>
              </div>
            ) : (
              <div className="field-hint" style={{ textAlign: 'center' }}>
                把卡片放到读卡器上，这里会显示目标标签。
              </div>
            )}
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">写入状态</div>
                <div className="panel-sub">设备回传的实时进度</div>
              </div>
              <Pill tone={writeError ? 'red' : writeDone ? 'aqua' : writing ? 'amber' : 'muted'} dot={writing || writeDone}>
                {writeError ? '失败' : writeDone ? '完成' : writing ? '进行中' : '待命'}
              </Pill>
            </div>
            <div className="panel-body">
              <div className="write-progress">
                <div className="write-progress-head">
                  <span>
                    {write.phase === 'verifying'
                      ? '回读校验'
                      : write.phase === 'done'
                        ? '写入完成'
                        : write.phase === 'error'
                          ? '写入中止'
                          : '写入进度'}
                  </span>
                  <span className="mono">{write.pct}%</span>
                </div>
                <div className="write-progress-track">
                  <div
                    className="write-progress-fill"
                    style={{
                      width: `${write.phase === 'error' ? 100 : write.pct}%`,
                      background: write.phase === 'error' ? 'var(--red)' : undefined
                    }}
                  />
                </div>
                <div className="write-steps">
                  <span className={`write-step ${writing || writeDone ? 'done' : ''}`}>
                    <BadgeCheck size={11} /> 准备
                  </span>
                  <span className={`write-step ${write.phase === 'writing' ? 'active' : writing || writeDone ? 'done' : ''}`}>
                    <PenLine size={11} /> 写入
                  </span>
                  <span className={`write-step ${write.phase === 'verifying' ? 'active' : writeDone ? 'done' : ''}`}>
                    <Braces size={11} /> 校验
                  </span>
                </div>
                {write.message && <div className="field-hint">{write.message}</div>}
                {writeError && write.code && <div className="field-error">错误码：{write.code}</div>}
              </div>

              <div className="info-banner" style={{ marginTop: 12 }}>
                <Sparkles size={15} />
                <span>写入期间请保持卡片贴在读卡器上；完成后会自动回读并刷新卡片信息。</span>
              </div>
            </div>
          </div>

          <div className="panel">
            <div className="panel-body" style={{ padding: 18 }}>
              <div className="row gap-8" style={{ marginBottom: 8 }}>
                <ShieldAlert size={15} style={{ color: 'var(--yellow-strong)' }} />
                <span className="strong small">资源卡小贴士</span>
              </div>
              <div className="stack gap-6 tiny muted" style={{ lineHeight: 1.7 }}>
                <span>· 卡片只保存资源路径，不复制音乐/故事文件。</span>
                <span>· 资源改名或删除后，旧卡片会失效，需要重新同步并写卡。</span>
                <span>· NTAG213 可用约 144 B；路径较长时请用 NTAG215/216。</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}

function PreviewRecord({ record }: { record: NdefRecord }) {
  return (
    <div className="preview-record">
      <div className="preview-record-head">
        <span>{record.typeLabel}</span>
        <span>
          TNF {record.tnf} · {record.payloadHex.length / 2} B
        </span>
      </div>
      <div className="preview-record-value">
        {record.kind === 'empty' ? '空记录' : record.value || '（无文本内容）'}
      </div>
    </div>
  )
}
