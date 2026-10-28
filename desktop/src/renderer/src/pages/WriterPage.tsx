import { useEffect, useMemo, useState } from 'react'
import {
  ArrowRight,
  BadgeCheck,
  Braces,
  ChevronDown,
  ChevronUp,
  ClipboardPenLine,
  Link2,
  Nfc,
  PenLine,
  RefreshCw,
  ShieldAlert,
  Sparkles,
  Type,
  Wifi
} from 'lucide-react'
import type { NdefParse, NdefRecord } from '@shared/types'
import { PageHeader } from '../components/PageHeader'
import { CardVisual } from '../components/CardVisual'
import { Button, InfoBanner, Pill, Segmented, Toggle } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { capacityLabel, kindAccent } from '../lib/card'
import {
  buildNdefMessage,
  bytesToHex,
  estimateTlvSize,
  hexToBytes,
  parseNdefMessage
} from '../lib/ndef'

type Mode = 'text' | 'uri' | 'custom'

const TEMPLATES = [
  { id: 'story', label: '故事卡片', icon: <Type size={12} /> },
  { id: 'music', label: '音乐链接', icon: <Link2 size={12} /> },
  { id: 'url', label: '网址', icon: <Wifi size={12} /> }
]

function applyTemplate(id: string): { mode: Mode; text: string; uri: string } | null {
  switch (id) {
    case 'story':
      return { mode: 'text', text: '月亮邮递员 · 第 03 集\n今晚的故事，送给你。', uri: '' }
    case 'music':
      return { mode: 'uri', text: '', uri: 'https://storybox.local/album/nursery' }
    case 'url':
      return { mode: 'uri', text: '', uri: 'https://example.com/story' }
    default:
      return null
  }
}

export function WriterPage() {
  const card = useAppStore((s) => s.card)
  const write = useAppStore((s) => s.write)
  const demo = useAppStore((s) => s.demo)
  const setView = useAppStore((s) => s.setView)
  const writeText = useAppStore((s) => s.writeText)
  const writeUri = useAppStore((s) => s.writeUri)
  const writeCustom = useAppStore((s) => s.writeCustom)
  const writeRaw = useAppStore((s) => s.writeRaw)
  const refresh = useAppStore((s) => s.refresh)
  const demoPlace = useAppStore((s) => s.demoPlaceCard)

  const [mode, setMode] = useState<Mode>('text')
  const [text, setText] = useState('月亮邮递员 · 第 03 集\n今晚的故事，送给你。')
  const [language, setLanguage] = useState('zh')
  const [uri, setUri] = useState('https://storybox.local/album/nursery')
  const [customHex, setCustomHex] = useState('')
  const [verify, setVerify] = useState(true)
  const [showRaw, setShowRaw] = useState(false)

  const [rawKind, setRawKind] = useState<'page' | 'block'>('page')
  const [rawStart, setRawStart] = useState('4')
  const [rawData, setRawData] = useState('00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F')
  const [rawKey, setRawKey] = useState('FFFFFFFFFFFF')

  useEffect(() => {
    const template = localStorage.getItem('nfc-studio.writer-template')
    if (!template) return
    localStorage.removeItem('nfc-studio.writer-template')
    const applied = applyTemplate(template)
    if (applied) {
      setMode(applied.mode)
      setText(applied.text)
      setUri(applied.uri)
    }
  }, [])

  useEffect(() => {
    if (!card) return
    setRawKind(card.type2 ? 'page' : 'block')
    setRawStart(card.type2 ? '4' : '1')
  }, [card?.type2])

  const message = useMemo<Uint8Array | null>(() => {
    try {
      if (mode === 'text') {
        if (!text.trim()) return null
        return buildNdefMessage([{ kind: 'text', text, language }])
      }
      if (mode === 'uri') {
        if (!uri.trim()) return null
        return buildNdefMessage([{ kind: 'uri', uri }])
      }
      if (!customHex.trim()) return null
      return hexToBytes(customHex)
    } catch {
      return null
    }
  }, [mode, text, language, uri, customHex])

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
  const canWrite =
    Boolean(card) &&
    card?.type2 === true &&
    card?.writable === true &&
    Boolean(message) &&
    !over &&
    !['start', 'writing', 'verifying'].includes(write.phase)

  const writing = ['start', 'writing', 'verifying'].includes(write.phase)
  const writeDone = write.phase === 'done'
  const writeError = write.phase === 'error'

  useEffect(() => {
    if (mode === 'custom' && customHex === '' && card?.ndef.ok && card.ndef.messageHex) {
      setCustomHex(card.ndef.messageHex)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [mode])

  function applyTemplateById(id: string): void {
    const applied = applyTemplate(id)
    if (!applied) return
    setMode(applied.mode)
    setText(applied.text)
    setUri(applied.uri)
  }

  function copyFromCard(): void {
    if (!card) return
    const record = card.ndef.records[0]
    if (!record) return
    if (record.kind === 'text') {
      setMode('text')
      setText(record.value)
      setLanguage(record.language || 'zh')
    } else if (record.kind === 'uri') {
      setMode('uri')
      setUri(record.uri || record.value)
    } else {
      setMode('custom')
      setCustomHex(card.ndef.messageHex)
    }
  }

  async function submit(): Promise<void> {
    if (mode === 'text') await writeText(text, language, verify)
    else if (mode === 'uri') await writeUri(uri, verify)
    else await writeCustom(customHex, verify)
  }

  async function submitRaw(): Promise<void> {
    if (!card) return
    await writeRaw(rawKind, Number(rawStart) || 0, rawData.replace(/[^0-9a-fA-F]/g, ''), rawKind === 'block' ? rawKey : undefined)
  }

  return (
    <div className="page">
      <PageHeader
        view="writer"
        actions={
          <>
            <Button
              variant="secondary"
              icon={<ClipboardPenLine size={14} />}
              onClick={copyFromCard}
              disabled={!card?.ndef.ok}
            >
              复制当前卡片
            </Button>
            <Button variant="secondary" icon={<RefreshCw size={14} />} onClick={() => void refresh()} disabled={!card}>
              刷新卡片
            </Button>
          </>
        }
      />

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
              <button type="button" className="btn btn-ghost btn-sm" style={{ marginLeft: 10 }} onClick={() => setView('reader')}>
                打开读卡器 <ArrowRight size={12} />
              </button>
            )}
          </InfoBanner>
        </div>
      )}

      {card && !card.type2 && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="warn" icon={<ShieldAlert size={16} />}>
            {card.type} 不支持标准 NDEF 写入。下方仍可以用高级原始块写入，默认密钥 FFFFFFFFFFFF。
          </InfoBanner>
        </div>
      )}

      <div className="writer-grid">
        <div className="writer-main">
          <div className="panel writer-form-panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">写入内容</div>
                <div className="panel-sub">内容会编码成标准 NDEF，手机与设备都能读取</div>
              </div>
              {card && <Pill tone={kindAccent(card.kind) as 'aqua'} dot>{card.type}</Pill>}
            </div>

            <div className="panel-body">
              <section>
                <div className="form-section-title">
                  <span className="step-num">01</span>
                  <h3>选择内容类型</h3>
                  <span className="form-section-sub">TNF 1 · Well Known</span>
                </div>

                <Segmented
                  value={mode}
                  onChange={(next) => setMode(next)}
                  options={[
                    { value: 'text', label: '文本', icon: <Type size={12} /> },
                    { value: 'uri', label: '链接', icon: <Link2 size={12} /> },
                    { value: 'custom', label: '自定义 NDEF', icon: <Braces size={12} /> }
                  ]}
                />

                <div className="template-row">
                  {TEMPLATES.map((template) => (
                    <button
                      key={template.id}
                      type="button"
                      className="template-chip"
                      onClick={() => applyTemplateById(template.id)}
                    >
                      {template.icon}
                      {template.label}
                    </button>
                  ))}
                </div>

                <div className="form-grid" style={{ marginTop: 18 }}>
                  {mode === 'text' && (
                    <>
                      <div className="field span-2">
                        <label className="field-label" htmlFor="write-text">
                          <span>
                            文本内容 <span className="req">*</span>
                          </span>
                          <span className={`char-count ${over ? 'over' : ''}`}>{text.length} 字符</span>
                        </label>
                        <textarea
                          id="write-text"
                          className="textarea"
                          value={text}
                          onChange={(event) => setText(event.target.value)}
                          placeholder="输入要写入卡片的文字…"
                          maxLength={500}
                        />
                        <div className="field-hint">UTF-8 编码，支持中文、emoji 与换行。</div>
                      </div>
                      <div className="field">
                        <label className="field-label" htmlFor="write-lang">
                          语言代码
                        </label>
                        <select
                          id="write-lang"
                          className="select"
                          value={language}
                          onChange={(event) => setLanguage(event.target.value)}
                        >
                          <option value="zh">zh · 中文</option>
                          <option value="en">en · English</option>
                          <option value="ja">ja · 日本語</option>
                          <option value="ko">ko · 한국어</option>
                        </select>
                        <div className="field-hint">NDEF Text 记录的语言标识</div>
                      </div>
                    </>
                  )}

                  {mode === 'uri' && (
                    <>
                      <div className="field span-2">
                        <label className="field-label" htmlFor="write-uri">
                          <span>
                            链接地址 <span className="req">*</span>
                          </span>
                        </label>
                        <input
                          id="write-uri"
                          className="input mono"
                          value={uri}
                          onChange={(event) => setUri(event.target.value)}
                          placeholder="https://example.com/story"
                        />
                        <div className="field-hint">
                          会自动选择最短的 URI 前缀编码（http、https、tel、mailto 等）。
                        </div>
                      </div>
                    </>
                  )}

                  {mode === 'custom' && (
                    <>
                      <div className="field span-2">
                        <label className="field-label" htmlFor="write-custom">
                          <span>
                            NDEF 消息 HEX <span className="req">*</span>
                          </span>
                          <span className="char-count">{message?.length ?? 0} B</span>
                        </label>
                        <textarea
                          id="write-custom"
                          className="textarea mono"
                          value={customHex}
                          onChange={(event) => setCustomHex(event.target.value)}
                          placeholder="D1010E5502..."
                          style={{ minHeight: 110, fontSize: 11 }}
                        />
                        <div className="field-hint">
                          直接填写完整的 NDEF 消息字节；适合写入 MIME、外部类型等高级记录。
                        </div>
                      </div>
                    </>
                  )}

                  <div className="span-2">
                    <div className="capacity-card">
                      <div className="capacity-head">
                        <span>标签容量</span>
                        <span className={over ? 'over' : ''}>
                          {needed} / {capacity || '—'} B
                        </span>
                      </div>
                      <div className="capacity-track">
                        <div
                          className={`capacity-fill ${over ? 'over' : needed / Math.max(capacity, 1) > 0.8 ? 'warn' : ''}`}
                          style={{ width: `${Math.min(100, capacity ? (needed / capacity) * 100 : 0)}%` }}
                        />
                      </div>
                      <div className="capacity-foot">
                        <span>NDEF TLV + 终止符</span>
                        <span>{over ? '超出容量，请缩短内容' : capacity ? `${Math.max(0, capacity - needed)} B 可用` : '等待卡片'}</span>
                      </div>
                    </div>
                  </div>
                </div>
              </section>

              <hr className="divider" />

              <section>
                <div className="form-section-title">
                  <span className="step-num">02</span>
                  <h3>写入选项</h3>
                  <span className="form-section-sub">写入前自动格式化未初始化的标签</span>
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
              </section>

              <hr className="divider" />

              <section>
                <button
                  type="button"
                  className="row gap-10"
                  style={{ border: 0, background: 'transparent', padding: 0, width: '100%', textAlign: 'left' }}
                  onClick={() => setShowRaw(!showRaw)}
                >
                  <span className="step-num" style={{ background: 'var(--surface-3)', color: 'var(--ink-2)' }}>
                    03
                  </span>
                  <span className="flex-1">
                    <span style={{ display: 'block', fontSize: 13, fontWeight: 750 }}>高级 · 原始页 / 块写入</span>
                    <span style={{ display: 'block', marginTop: 2, fontSize: 10.5, color: 'var(--ink-3)' }}>
                      绕过 NDEF，直接操作存储区。写错可能让标签不可读。
                    </span>
                  </span>
                  {showRaw ? <ChevronUp size={16} /> : <ChevronDown size={16} />}
                </button>

                {showRaw && (
                  <div style={{ marginTop: 16 }}>
                    <div className="raw-grid">
                      <div className="field">
                        <span className="field-label">单位</span>
                        <select
                          className="select"
                          value={rawKind}
                          onChange={(event) => setRawKind(event.target.value as 'page' | 'block')}
                        >
                          <option value="page">Page</option>
                          <option value="block">Block</option>
                        </select>
                      </div>
                      <div className="field">
                        <span className="field-label">起始地址</span>
                        <input
                          className="input mono"
                          type="number"
                          value={rawStart}
                          onChange={(event) => setRawStart(event.target.value)}
                          min={0}
                        />
                      </div>
                      <div className="field">
                        <span className="field-label">{rawKind === 'page' ? '4 字节页数据' : '16 字节块数据'}</span>
                        <textarea
                          className="textarea mono"
                          value={rawData}
                          onChange={(event) => setRawData(event.target.value)}
                          style={{ minHeight: 64, fontSize: 11 }}
                        />
                      </div>
                      {rawKind === 'block' && (
                        <div className="field" style={{ gridColumn: 'span 2' }}>
                          <span className="field-label">Classic 密钥 (Key A, 12 位 HEX)</span>
                          <input
                            className="input mono"
                            value={rawKey}
                            onChange={(event) => setRawKey(event.target.value)}
                            maxLength={12}
                          />
                        </div>
                      )}
                      <div className="field">
                        <Button
                          variant="danger"
                          size="sm"
                          type="button"
                          onClick={() => void submitRaw()}
                          disabled={!card || !card.writable || writing}
                          icon={<ShieldAlert size={13} />}
                        >
                          执行原始写入
                        </Button>
                      </div>
                    </div>
                    <div className="field-hint" style={{ marginTop: 8 }}>
                      {rawKind === 'page'
                        ? 'Type 2：从 Page 4 开始是用户区，Page 3 是 CC，请勿覆盖配置页。'
                        : 'MIFARE Classic：每 4 块一个扇区，扇区尾块（Block 3 / 7 / …）是密钥区，固件会拒绝写入。'}
                    </div>
                  </div>
                )}
              </section>
            </div>

            <div className="write-actions">
              <div className="write-hint">
                {!card
                  ? '放置卡片后即可写入。'
                  : !card.type2
                    ? '当前卡片不支持 NDEF 写入，请使用高级原始块写入。'
                    : !card.writable
                      ? '当前标签只读，无法写入。'
                      : over
                        ? '内容超过标签容量，请缩短后重试。'
                        : `将写入 ${needed} B，剩余 ${Math.max(0, capacity - needed)} B。写入过程请保持卡片不动。`}
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
                <div className="empty-mini">填写内容后这里会显示编码结果</div>
              ) : (
                <>
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
                  <span>{write.phase === 'verifying' ? '回读校验' : write.phase === 'done' ? '写入完成' : write.phase === 'error' ? '写入中止' : '写入进度'}</span>
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
                <span className="strong small">写卡小贴士</span>
              </div>
              <div className="stack gap-6 tiny muted" style={{ lineHeight: 1.7 }}>
                <span>· NTAG213 可用约 144 B；写 URL 会使用前缀压缩。</span>
                <span>· 已锁定的标签会显示为只读，写入前请确认。</span>
                <span>· 写入标准 NDEF 后，手机贴近即可读取。</span>
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
