import { useState } from 'react'
import type { NdefParse, NdefRecord } from '@shared/types'
import { Braces, FileText, Inbox, Link2, MessageSquareText, Puzzle, Type } from 'lucide-react'
import { CopyButton, EmptyState, Pill } from './ui'

function RecordIcon({ record }: { record: NdefRecord }) {
  switch (record.kind) {
    case 'text':
      return <Type size={16} />
    case 'uri':
      return <Link2 size={16} />
    case 'mime':
      return <FileText size={16} />
    case 'external':
      return <Puzzle size={16} />
    default:
      return <Braces size={16} />
  }
}

export function NdefRecords({
  parse,
  onWrite,
  compact = false
}: {
  parse: NdefParse
  onWrite?: () => void
  compact?: boolean
}) {
  const [expanded, setExpanded] = useState<string | null>(null)

  if (!parse.ok || parse.records.length === 0) {
    return (
      <EmptyState
        icon={<Inbox size={26} />}
        title={parse.formatted ? '这张卡片还没有 NDEF 内容' : '标签尚未格式化为 NDEF'}
        description={parse.error ?? '写入一条文本或链接，手机靠近即可读取。'}
        actions={
          onWrite ? (
            <button type="button" className="btn btn-primary btn-sm" onClick={onWrite}>
              <MessageSquareText size={13} /> 写入内容
            </button>
          ) : undefined
        }
      />
    )
  }

  return (
    <div className="ndef-list">
      {parse.records.map((record) => (
        <div className="ndef-record" key={record.id}>
          <span className="ndef-icon">
            <RecordIcon record={record} />
          </span>
          <div className="ndef-body">
            <div className="ndef-head">
              <span className="ndef-kind">{record.kind === 'text' ? '文本记录' : record.kind === 'uri' ? '链接记录' : record.typeLabel}</span>
              <Pill tone="muted">TNF {record.tnf}</Pill>
              {record.language && <Pill tone="aqua">{record.language}</Pill>}
              <span style={{ flex: 1 }} />
              <CopyButton value={record.value} label="复制内容" />
            </div>
            {record.kind === 'uri' ? (
              <a className="ndef-value uri" href={record.uri ?? record.value} target="_blank" rel="noreferrer">
                {record.value}
              </a>
            ) : record.kind === 'empty' ? (
              <div className="ndef-value faint">空记录</div>
            ) : (
              <div className="ndef-value" style={{ whiteSpace: 'pre-wrap' }}>
                {record.value}
              </div>
            )}
            <div className="ndef-meta">
              type <span className="mono">{record.type || '—'}</span> · payload {record.payloadHex.length / 2} B
            </div>
            {!compact && (
              <>
                <button
                  type="button"
                  className="code-toggle"
                  onClick={() => setExpanded(expanded === record.id ? null : record.id)}
                >
                  <Braces size={11} /> {expanded === record.id ? '收起原始负载' : '查看原始负载'}
                </button>
                {expanded === record.id && (
                  <div className="code-block">{record.payloadHex.match(/.{1,32}/g)?.join('\n')}</div>
                )}
              </>
            )}
          </div>
        </div>
      ))}
    </div>
  )
}
