import { useEffect, useState } from 'react'
import {
  ArrowDownToLine,
  ArrowRight,
  CircleAlert,
  Cpu,
  Layers,
  LayoutTemplate,
  Nfc,
  PenLine,
  ScanLine,
  Sparkles,
  Timer,
  TriangleAlert,
  Webhook
} from 'lucide-react'
import { PageHeader } from '../components/PageHeader'
import { NfcScanner } from '../components/NfcScanner'
import { CardVisual } from '../components/CardVisual'
import { ActivityList } from '../components/ActivityList'
import { Button, InfoBanner, Pill } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { capacityLabel, formatUid, kindAccent, relativeTime } from '../lib/card'
import { describeNdef } from '../lib/ndef'

function useMinutesSince(startedAt: number): string {
  const [now, setNow] = useState(Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 30_000)
    return () => clearInterval(timer)
  }, [])
  const minutes = Math.floor((now - startedAt) / 60_000)
  if (minutes < 1) return '<1'
  if (minutes < 60) return String(minutes)
  return `${Math.floor(minutes / 60)}h ${minutes % 60}m`
}

export function DashboardPage() {
  const nfc = useAppStore((s) => s.nfc)
  const connection = useAppStore((s) => s.connection)
  const device = useAppStore((s) => s.device)
  const card = useAppStore((s) => s.card)
  const stats = useAppStore((s) => s.stats)
  const activities = useAppStore((s) => s.activities)
  const history = useAppStore((s) => s.history)
  const demo = useAppStore((s) => s.demo)
  const setView = useAppStore((s) => s.setView)
  const connect = useAppStore((s) => s.connect)
  const refresh = useAppStore((s) => s.refresh)
  const demoPlace = useAppStore((s) => s.demoPlaceCard)
  const duration = useMinutesSince(stats.startedAt)

  const connected = connection.state === 'connected'
  const moduleReady = connected && nfc.present

  function goWriter(template?: string): void {
    if (template) localStorage.setItem('nfc-studio.writer-template', template)
    setView('writer')
  }

  return (
    <div className="page">
      <PageHeader
        view="dashboard"
        actions={
          <>
            <Button variant="secondary" icon={<ScanLine size={15} />} onClick={() => setView('reader')}>
              打开读卡器
            </Button>
            <Button variant="primary" icon={<PenLine size={15} />} onClick={() => goWriter()}>
              写一张卡
            </Button>
          </>
        }
      />

      {!connected && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner
            tone={connection.state === 'error' ? 'error' : 'warn'}
            icon={<Cpu size={16} />}
          >
            {connection.state === 'error'
              ? connection.message ?? '设备连接失败'
              : connection.state === 'connecting' || connection.state === 'scanning'
                ? `正在${connection.state === 'scanning' ? '扫描串口' : '连接设备'}…`
                : '尚未连接 StoryBox。插好 USB 后点击右侧按钮扫描串口；也可以在设置里开启演示模式先体验界面。'}
            {connection.state !== 'connecting' && connection.state !== 'scanning' && (
              <button
                type="button"
                className="btn btn-soft btn-sm"
                style={{ marginLeft: 10 }}
                onClick={() => void connect()}
              >
                扫描并连接
              </button>
            )}
          </InfoBanner>
        </div>
      )}

      {connected && !nfc.present && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="warn" icon={<TriangleAlert size={16} />}>
            串口已连接，但没有检测到 PN532。请检查模块 3.3V 供电、SDA/SCL 是否和 FRAM 并联、模块拨码是否在
            I2C 模式。
            <button
              type="button"
              className="btn btn-ghost btn-sm"
              style={{ marginLeft: 8 }}
              onClick={() => setView('device')}
            >
              去检查接线 <ArrowRight size={12} />
            </button>
          </InfoBanner>
        </div>
      )}

      <div className={`stat-grid`}>
        <div className="stat-card" style={{ ['--stat-tint' as string]: 'var(--aqua-soft)' }}>
          <div className="stat-label">
            <ScanLine size={12} /> 本次读卡
          </div>
          <div className="stat-value">
            {stats.cardsRead}
            <small>张</small>
          </div>
          <div className="stat-meta">本会话累计识别</div>
        </div>
        <div className="stat-card" style={{ ['--stat-tint' as string]: 'var(--aqua-soft)' }}>
          <div className="stat-label">
            <ArrowDownToLine size={12} /> 写入成功
          </div>
          <div className="stat-value">{stats.writesOk}</div>
          <div className="stat-meta">NDEF / 原始写入</div>
        </div>
        <div className="stat-card" style={{ ['--stat-tint' as string]: 'var(--coral-soft)' }}>
          <div className="stat-label">
            <CircleAlert size={12} /> 写入失败
          </div>
          <div className="stat-value">{stats.writesFailed}</div>
          <div className="stat-meta">回读校验或卡片问题</div>
        </div>
        <div className="stat-card" style={{ ['--stat-tint' as string]: 'var(--yellow-soft)' }}>
          <div className="stat-label">
            <Timer size={12} /> 会话时长
          </div>
          <div className="stat-value">
            {duration}
            <small>{duration.endsWith('m') || duration.endsWith('h') ? '' : 'min'}</small>
          </div>
          <div className="stat-meta">{history.length} 张卡片在库</div>
        </div>
      </div>

      <div className="hero-grid" style={{ marginTop: 18 }}>
        <NfcScanner />

        <div className="hero-side">
          <div className="panel module-card">
            <div className="module-head">
              <span className="module-logo">
                <Cpu size={20} />
              </span>
              <div className="module-copy">
                <div className="module-name">NXP PN532</div>
                <div className="module-sub">13.56 MHz · I2C 读卡模块</div>
              </div>
              <span className={`module-led ${moduleReady ? 'on' : connected ? 'warn' : ''}`} />
            </div>

            <div className="module-stats">
              <div className="module-stat">
                <div className="module-stat-label">模块状态</div>
                <div className="module-stat-value">{moduleReady ? '在线' : connected ? '未检测' : '离线'}</div>
              </div>
              <div className="module-stat">
                <div className="module-stat-label">固件版本</div>
                <div className="module-stat-value">{nfc.present ? nfc.fw : '--'}</div>
              </div>
              <div className="module-stat">
                <div className="module-stat-label">I2C 地址</div>
                <div className="module-stat-value">{nfc.i2c}</div>
              </div>
              <div className="module-stat">
                <div className="module-stat-label">轮询</div>
                <div className="module-stat-value">{nfc.polling ? '运行中' : '已暂停'}</div>
              </div>
            </div>

            {!connected ? (
              <div className="module-warning" style={{ marginTop: 14 }}>
                <TriangleAlert size={14} style={{ flex: '0 0 14px', marginTop: 1 }} />
                <span>尚未连接串口设备。连接后可自动识别 PN532 与卡片。</span>
              </div>
            ) : !nfc.present ? (
              <div className="module-warning" style={{ marginTop: 14 }}>
                <TriangleAlert size={14} style={{ flex: '0 0 14px', marginTop: 1 }} />
                <span>模块没有响应。请检查 I2C 接线与拨码后重新识别。</span>
              </div>
            ) : null}

            <div className="row gap-8" style={{ marginTop: 14 }}>
              <Button variant="secondary" size="sm" onClick={() => setView('device')} icon={<Cpu size={13} />}>
                设备详情
              </Button>
              <Button
                variant="ghost"
                size="sm"
                onClick={() => void refresh()}
                disabled={!connected}
                icon={<ScanLine size={13} />}
              >
                重新检测
              </Button>
              <span style={{ flex: 1 }} />
              <Pill tone={moduleReady ? 'aqua' : 'muted'} dot={moduleReady}>
                {device?.name ?? 'StoryBox'}
              </Pill>
            </div>
          </div>

          <div className="panel quick-panel">
            <div className="row gap-8">
              <div className="flex-1">
                <div className="quick-title">{card ? '继续写这张卡' : '快速写一张卡'}</div>
                <div className="quick-sub">
                  {card ? `${card.type} · 剩余约 ${Math.max(0, card.cap - (card.ndef.messageHex.length / 2 + 4))} B` : '选择模板，几步完成 NDEF 写入'}
                </div>
              </div>
              <Sparkles size={18} style={{ color: 'var(--yellow-strong)' }} />
            </div>

            <div className="quick-templates">
              <button type="button" className="template-chip" onClick={() => goWriter('story')}>
                <LayoutTemplate size={12} /> 故事卡片
              </button>
              <button type="button" className="template-chip" onClick={() => goWriter('music')}>
                <Nfc size={12} /> 音乐链接
              </button>
              <button type="button" className="template-chip" onClick={() => goWriter('url')}>
                <Webhook size={12} /> 网址
              </button>
            </div>

            <Button
              variant="primary"
              className="btn-block"
              onClick={() => goWriter()}
              icon={<PenLine size={14} />}
            >
              {card ? '写入当前卡片' : '开始写卡'}
            </Button>
            {!card && demo && (
              <button
                type="button"
                className="btn btn-ghost btn-sm btn-block"
                style={{ marginTop: 8 }}
                onClick={demoPlace}
              >
                先模拟一张卡片
              </button>
            )}
          </div>
        </div>
      </div>

      <div className="dashboard-split">
        <div className="panel">
          <div className="panel-head">
            <div className="panel-title-row">
              <div className="panel-title">最近活动</div>
              <div className="panel-sub">读卡、写入与设备连接都会出现在这里</div>
            </div>
            <button type="button" className="btn btn-ghost btn-sm" onClick={() => setView('library')}>
              卡片库 <ArrowRight size={12} />
            </button>
          </div>
          <ActivityList activities={activities} limit={7} />
        </div>

        <div className="panel">
          <div className="panel-head">
            <div className="panel-title-row">
              <div className="panel-title">当前卡片</div>
              <div className="panel-sub">{card ? `最近更新 ${relativeTime(card.lastSeen)}` : '等待卡片进入射频场'}</div>
            </div>
            {card && <Pill tone={kindAccent(card.kind) as 'aqua'} dot>{card.type}</Pill>}
          </div>
          <div className="panel-body">
            {card ? (
              <div className="stack gap-14" style={{ gap: 14 }}>
                <CardVisual card={card} size="sm" />
                <div className="detail-list">
                  <div className="detail-row">
                    <span className="detail-key">UID</span>
                    <span className="detail-val mono">{formatUid(card.uid)}</span>
                  </div>
                  <div className="detail-row">
                    <span className="detail-key">NDEF 内容</span>
                    <span className="detail-val">{describeNdef(card.ndef)}</span>
                  </div>
                  <div className="detail-row">
                    <span className="detail-key">容量</span>
                    <span className="detail-val">
                      {capacityLabel(card)} · {card.ndef.records.length} 条记录
                    </span>
                  </div>
                </div>
                <Button variant="secondary" size="sm" onClick={() => setView('reader')} icon={<ScanLine size={13} />}>
                  查看完整信息
                </Button>
              </div>
            ) : (
              <div className="empty-mini" style={{ padding: '28px 12px' }}>
                <Nfc size={26} style={{ opacity: 0.5 }} />
                <span>还没有检测到卡片</span>
                {demo ? (
                  <Button variant="soft" size="sm" onClick={demoPlace}>
                    模拟放置一张
                  </Button>
                ) : (
                  <Button variant="secondary" size="sm" onClick={() => setView('reader')}>
                    打开读卡器
                  </Button>
                )}
              </div>
            )}
          </div>
        </div>
      </div>

      <div className="extension-strip">
        <div className="extension-mini">
          <span className="extension-mini-icon">
            <Layers size={16} />
          </span>
          <div>
            <div className="extension-mini-title">批量写卡</div>
            <div className="extension-mini-sub">队列化写入与失败重试，扩展中心已预留插槽。</div>
          </div>
          <Pill tone="muted">规划中</Pill>
        </div>
        <div className="extension-mini">
          <span className="extension-mini-icon">
            <LayoutTemplate size={16} />
          </span>
          <div>
            <div className="extension-mini-title">模板库</div>
            <div className="extension-mini-sub">故事、专辑、网址模板，一键填入写卡表单。</div>
          </div>
          <Pill tone="aqua">已可用</Pill>
        </div>
        <div className="extension-mini">
          <span className="extension-mini-icon">
            <Webhook size={16} />
          </span>
          <div>
            <div className="extension-mini-title">HTTP / MQTT 联动</div>
            <div className="extension-mini-sub">设备能力通过协议暴露，后续接入自动化流程。</div>
          </div>
          <Pill tone="muted">规划中</Pill>
        </div>
      </div>
    </div>
  )
}
