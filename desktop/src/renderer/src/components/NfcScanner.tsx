import { Cable, Nfc, Pause, Play, RotateCw, TriangleAlert } from 'lucide-react'
import { useAppStore } from '../store/useAppStore'
import { CardVisual } from './CardVisual'
import { Pill } from './ui'

export function NfcScanner({
  className = '',
  compact = false,
  showActions = true
}: {
  className?: string
  compact?: boolean
  showActions?: boolean
}) {
  const card = useAppStore((s) => s.card)
  const nfc = useAppStore((s) => s.nfc)
  const connection = useAppStore((s) => s.connection)
  const demo = useAppStore((s) => s.demo)
  const setPolling = useAppStore((s) => s.setPolling)
  const refresh = useAppStore((s) => s.refresh)
  const demoPlace = useAppStore((s) => s.demoPlaceCard)
  const demoRemove = useAppStore((s) => s.demoRemoveCard)
  const setView = useAppStore((s) => s.setView)

  const connected = connection.state === 'connected'
  const live = connected && nfc.present && nfc.polling

  const emptyTitle = !connected ? '设备未连接' : !nfc.present ? '未检测到 PN532' : '把卡片靠近天线'
  const emptySub = !connected
    ? '在设备页选择 COM 口并连接 StoryBox，随后即可自动寻卡。'
    : !nfc.present
      ? '检查模块 3.3V 供电、SDA/SCL 接线与 I2C 拨码后再试。'
      : 'NTAG / MIFARE 卡片放在 PN532 线圈上方 2–3 cm 内'

  return (
    <section className={`scanner-panel ${live ? 'polling' : ''} ${className}`.trim()}>
      <div className="scanner-head">
        <div className="scanner-title-group">
          <div className="scanner-kicker">Live Reader</div>
          <div className="scanner-title">PN532 · I2C 读卡器</div>
        </div>
        <Pill tone={live ? 'aqua' : connected ? 'amber' : 'muted'} dot={live}>
          {live ? '轮询中' : !connected ? '设备离线' : !nfc.present ? '模块离线' : '已暂停'}
        </Pill>
      </div>

      <div className="scanner-body">
        <div className="scanner-stage">
          <div className="orb">
            <span className="orb-ring" />
            <span className="orb-ring" />
            <span className="orb-ring" />
            <span className="orb-ring" />
            <span className="orb-core" />
          </div>
          <span className="scanline" />
          {card ? (
            <div className="scanner-card">
              <CardVisual card={card} size={compact ? 'sm' : 'md'} />
            </div>
          ) : (
            <div className="scanner-empty">
              <div className="scanner-empty-icon">
                {!connected ? (
                  <Cable size={34} strokeWidth={1.5} />
                ) : !nfc.present ? (
                  <TriangleAlert size={34} strokeWidth={1.5} />
                ) : (
                  <Nfc size={38} strokeWidth={1.4} />
                )}
              </div>
              <div className="scanner-empty-title">{emptyTitle}</div>
              <div className="scanner-empty-sub">{emptySub}</div>
              {!connected && (
                <button type="button" className="scanner-btn" onClick={() => setView('device')}>
                  <Cable size={12} /> 打开设备页
                </button>
              )}
              {connected && !nfc.present && (
                <button type="button" className="scanner-btn" onClick={() => setView('device')}>
                  <TriangleAlert size={12} /> 查看接线排查
                </button>
              )}
              {demo && connected && nfc.present && (
                <button type="button" className="scanner-btn" onClick={demoPlace}>
                  <Nfc size={12} /> 模拟放置卡片
                </button>
              )}
            </div>
          )}
        </div>
      </div>

      <div className="scanner-foot">
        <span className="live-chip">
          <span className="dot" />
          {live ? 'LIVE' : 'IDLE'}
        </span>
        <span className="scanner-meta">I2C 0x24 · 13.56 MHz</span>
        {showActions && (
          <div className="scanner-actions">
            {demo && connected && nfc.present && (
              <button
                type="button"
                className="scanner-btn"
                onClick={card ? demoRemove : demoPlace}
                title={card ? '模拟卡片离开' : '模拟放置卡片'}
              >
                {card ? '移开卡片' : '放置卡片'}
              </button>
            )}
            <button
              type="button"
              className="scanner-btn"
              onClick={() => void setPolling(!nfc.polling)}
              disabled={!connected || !nfc.present}
            >
              {nfc.polling ? <Pause size={12} /> : <Play size={12} />}
              {nfc.polling ? '暂停' : '开始'}
            </button>
            <button
              type="button"
              className="scanner-btn"
              onClick={() => void refresh()}
              disabled={!connected}
              title="重新读取卡片"
            >
              <RotateCw size={12} />
              刷新
            </button>
          </div>
        )}
      </div>
    </section>
  )
}
