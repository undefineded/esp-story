import { useEffect, useMemo, useRef, useState } from 'react'
import {
  Cable,
  CheckCircle2,
  Copy,
  Cpu,
  Gauge,
  Pause,
  Play,
  PlugZap,
  RefreshCw,
  ScrollText,
  ShieldCheck,
  Terminal,
  Trash2,
  TriangleAlert,
  Unplug,
  Usb
} from 'lucide-react'
import { PageHeader } from '../components/PageHeader'
import { Button, CopyButton, EmptyState, InfoBanner, Pill, Toggle } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { clockTime, formatBytes } from '../lib/card'

type LogFilter = 'all' | 'in' | 'out' | 'sys' | 'error'

export function DevicePage() {
  const connection = useAppStore((s) => s.connection)
  const device = useAppStore((s) => s.device)
  const nfc = useAppStore((s) => s.nfc)
  const ports = useAppStore((s) => s.ports)
  const logs = useAppStore((s) => s.logs)
  const busy = useAppStore((s) => s.busy)
  const settings = useAppStore((s) => s.settings)
  const scanPorts = useAppStore((s) => s.scanPorts)
  const connect = useAppStore((s) => s.connect)
  const disconnect = useAppStore((s) => s.disconnect)
  const setPolling = useAppStore((s) => s.setPolling)
  const refresh = useAppStore((s) => s.refresh)
  const updateSettings = useAppStore((s) => s.updateSettings)
  const clearLogs = useAppStore((s) => s.clearLogs)
  const pingDevice = useAppStore((s) => s.pingDevice)
  const showToast = useAppStore((s) => s.showToast)

  const [selectedPort, setSelectedPort] = useState(connection.port ?? ports[0]?.path ?? '')
  const [logFilter, setLogFilter] = useState<LogFilter>('all')
  const [autoScroll, setAutoScroll] = useState(true)
  const logRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (connection.port) setSelectedPort(connection.port)
  }, [connection.port])

  useEffect(() => {
    if (!selectedPort && ports.length > 0) setSelectedPort(ports[0].path)
  }, [ports, selectedPort])

  const filteredLogs = useMemo(() => {
    if (logFilter === 'all') return logs.slice(-300)
    return logs.filter((entry) => entry.dir === logFilter).slice(-300)
  }, [logs, logFilter])

  useEffect(() => {
    if (autoScroll && logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight
  }, [filteredLogs, autoScroll])

  const connected = connection.state === 'connected'
  const port = ports.find((item) => item.path === selectedPort)

  async function ping(): Promise<void> {
    const result = await pingDevice()
    if (result) {
      showToast('success', 'Pong', `运行 ${Math.floor(result.uptime / 1000)} 秒 · 空闲堆 ${formatBytes(result.heap)}`)
    }
  }

  function copyLogs(): void {
    const text = filteredLogs
      .map((entry) => `${clockTime(entry.ts)} [${entry.dir.toUpperCase()}] ${entry.text}`)
      .join('\n')
    void navigator.clipboard.writeText(text).then(() => showToast('info', '日志已复制'))
  }

  return (
    <div className="page">
      <PageHeader
        view="device"
        actions={
          <>
            <Button variant="secondary" icon={<RefreshCw size={14} />} onClick={() => void scanPorts()}>
              扫描串口
            </Button>
            <Button variant="secondary" icon={<Gauge size={14} />} onClick={() => void ping()} disabled={!connected}>
              Ping
            </Button>
            {connected ? (
              <Button variant="danger" icon={<Unplug size={14} />} onClick={() => void disconnect()} loading={busy}>
                断开连接
              </Button>
            ) : (
              <Button
                variant="primary"
                icon={<PlugZap size={14} />}
                onClick={() => void connect(selectedPort || undefined)}
                loading={busy || connection.state === 'connecting' || connection.state === 'scanning'}
              >
                连接
              </Button>
            )}
          </>
        }
      />

      {connection.state === 'error' && connection.message && (
        <div style={{ marginBottom: 16 }}>
          <InfoBanner tone="error" icon={<TriangleAlert size={16} />}>
            {connection.message}
          </InfoBanner>
        </div>
      )}

      <div className="device-grid">
        <div className="device-col">
          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <Usb size={15} /> 串口连接
                </div>
                <div className="panel-sub">选择 ESP32-S3 的 USB CDC 或 USB-UART 端口</div>
              </div>
              <Pill tone={connected ? 'aqua' : 'muted'} dot>
                {connected ? `已连接 ${connection.port}` : '未连接'}
              </Pill>
            </div>
            <div className="panel-body">
              {ports.length === 0 ? (
                <EmptyState
                  icon={<Cable size={24} />}
                  title="没有发现串口"
                  description="检查 USB 线是否支持数据传输，并关闭占用串口的串口监视器。"
                  actions={
                    <Button variant="secondary" size="sm" onClick={() => void scanPorts()}>
                      重新扫描
                    </Button>
                  }
                />
              ) : (
                <div className="port-list">
                  {ports.map((item) => (
                    <button
                      type="button"
                      key={item.path}
                      className={`port-item ${selectedPort === item.path ? 'active' : ''}`}
                      onClick={() => setSelectedPort(item.path)}
                      disabled={connected}
                    >
                      <span className="port-radio" />
                      <div className="port-copy">
                        <div className="port-path">{item.path}</div>
                        <div className="port-name">
                          {item.manufacturer ? `${item.manufacturer} · ` : ''}
                          {item.friendlyName || item.productId || '串口设备'}
                        </div>
                      </div>
                      {connection.port === item.path && connected && <Pill tone="aqua">当前</Pill>}
                      {item.vendorId?.toLowerCase() === '303a' && <Pill tone="blue">ESP32</Pill>}
                    </button>
                  ))}
                </div>
              )}

              <div className="row gap-16 wrap" style={{ marginTop: 16 }}>
                <Toggle
                  checked={settings.autoConnect}
                  onChange={(next) => void updateSettings({ autoConnect: next })}
                  title="自动连接"
                  sub="下次启动尝试连接上次端口"
                />
                <span style={{ flex: 1 }} />
                <span className="tiny faint">115200 baud · 8N1 · 换行 JSON</span>
              </div>

              {port && (
                <div className="info-banner" style={{ marginTop: 14 }}>
                  <Terminal size={15} />
                  <span>
                    {port.path} · {port.manufacturer || '未知厂商'} · VID {port.vendorId || '—'} / PID{' '}
                    {port.productId || '—'}
                  </span>
                </div>
              )}
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <Cpu size={15} /> 主板信息
                </div>
                <div className="panel-sub">由设备 hello 握手返回</div>
              </div>
              <Pill tone={connected ? 'aqua' : 'muted'}>{device?.board ?? 'ESP32-S3'}</Pill>
            </div>
            <div className="panel-body">
              <div className="device-info-list">
                <div className="info-tile">
                  <div className="info-label">设备名称</div>
                  <div className="info-value">{device?.name ?? '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">固件</div>
                  <div className="info-value">{device?.fw ?? '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">协议版本</div>
                  <div className="info-value">v{device?.proto ?? '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">运行时间</div>
                  <div className="info-value">{nfc.uptime ? `${Math.floor(nfc.uptime / 1000)} s` : '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">空闲堆</div>
                  <div className="info-value">{nfc.heap ? formatBytes(nfc.heap) : '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">角色</div>
                  <div className="info-value">{device?.role ?? '—'}</div>
                </div>
                <div className="info-tile wide">
                  <div className="info-label">设备能力</div>
                  <div className="feature-chips" style={{ marginTop: 5 }}>
                    {(device?.features ?? []).length > 0 ? (
                      device?.features.map((feature) => (
                        <Pill tone="blue" key={feature}>
                          {feature}
                        </Pill>
                      ))
                    ) : (
                      <span className="faint small">连接后显示</span>
                    )}
                  </div>
                </div>
              </div>
            </div>
          </div>
        </div>

        <div className="device-col">
          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <ShieldCheck size={15} /> PN532 模块
                </div>
                <div className="panel-sub">I2C 0x24 · 与 FRAM 共用 SDA / SCL</div>
              </div>
              <span className={`module-led ${nfc.present ? 'on' : connected ? 'warn' : ''}`} />
            </div>
            <div className="panel-body">
              <div className="row gap-16" style={{ alignItems: 'flex-start' }}>
                <span className="module-logo" style={{ width: 58, height: 58, borderRadius: 16 }}>
                  <ChipIcon present={nfc.present} />
                </span>
                <div className="flex-1">
                  <div className="row gap-8">
                    <span style={{ fontSize: 18, fontWeight: 750, fontFamily: 'var(--font-display)' }}>
                      {nfc.present ? 'PN532 在线' : 'PN532 未检测到'}
                    </span>
                    <Pill tone={nfc.present ? 'aqua' : 'red'} dot>
                      {nfc.present ? 'READY' : 'OFFLINE'}
                    </Pill>
                  </div>
                  <div className="small muted" style={{ marginTop: 4, lineHeight: 1.6 }}>
                    {nfc.present
                      ? `固件 ${nfc.fw}，轮询${nfc.polling ? '运行中' : '已暂停'}，卡片${nfc.busy ? '写入中' : '空闲'}。`
                      : '串口已连接，但模块没有返回固件版本。请按下方清单检查硬件。'}
                  </div>
                </div>
              </div>

              <div className="device-info-list" style={{ marginTop: 16 }}>
                <div className="info-tile">
                  <div className="info-label">芯片</div>
                  <div className="info-value">{nfc.chip}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">固件版本</div>
                  <div className="info-value">{nfc.present ? nfc.fw : '—'}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">I2C 地址</div>
                  <div className="info-value">{nfc.i2c}</div>
                </div>
                <div className="info-tile">
                  <div className="info-label">轮询状态</div>
                  <div className="info-value">{nfc.polling ? '运行中' : '已暂停'}</div>
                </div>
              </div>

              <div className="row gap-8 wrap" style={{ marginTop: 14 }}>
                <Button
                  variant="secondary"
                  size="sm"
                  icon={<RefreshCw size={13} />}
                  onClick={() => void refresh()}
                  disabled={!connected}
                >
                  重新检测
                </Button>
                <Button
                  variant="soft"
                  size="sm"
                  icon={nfc.polling ? <Pause size={13} /> : <Play size={13} />}
                  onClick={() => void setPolling(!nfc.polling)}
                  disabled={!connected}
                >
                  {nfc.polling ? '暂停轮询' : '恢复轮询'}
                </Button>
              </div>

              {!nfc.present && (
                <div style={{ marginTop: 18 }}>
                  <div className="info-label" style={{ marginBottom: 9 }}>
                    排查清单
                  </div>
                  <div className="troubleshoot">
                    {[
                      'PN532 VCC 接 3.3V（不要接 5V），GND 与主板共地。',
                      'SDA 接 IO1、SCL 接 IO2，与 FRAM 并联；确认模块上拉电阻存在。',
                      '模块拨码切到 I2C：SEL0 = ON，SEL1 = OFF（以模块丝印为准）。',
                      'I2C 地址固定 0x24；若总线被占用，关闭串口监视器 / 其它上位机。',
                      '重新上电后再点“重新检测”。'
                    ].map((item, index) => (
                      <div className="troubleshoot-item" key={item}>
                        <span className="idx">{index + 1}</span>
                        <span>{item}</span>
                      </div>
                    ))}
                  </div>
                </div>
              )}
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <Terminal size={15} /> USB 协议速览
                </div>
                <div className="panel-sub">新行分隔 JSON · v2</div>
              </div>
              <Pill tone="violet">可扩展</Pill>
            </div>
            <div className="panel-body">
              <table className="protocol-table">
                <tbody>
                  <tr>
                    <td style={{ width: 140 }}>
                      <code>sys · hello</code>
                    </td>
                    <td>握手，返回板卡、固件与能力列表</td>
                  </tr>
                  <tr>
                    <td>
                      <code>nfc · status</code>
                    </td>
                    <td>PN532 是否在线、轮询状态、当前卡片</td>
                  </tr>
                  <tr>
                    <td>
                      <code>nfc · read</code>
                    </td>
                    <td>读取页 / 块原始数据，支持 Classic 密钥</td>
                  </tr>
                  <tr>
                    <td>
                      <code>nfc · write_ndef</code>
                    </td>
                    <td>写入 NDEF 消息，T2T TLV 与 CC 由固件处理</td>
                  </tr>
                  <tr>
                    <td>
                      <code>nfc · write_raw</code>
                    </td>
                    <td>原始页 / 块写入，用于高级调试</td>
                  </tr>
                </tbody>
              </table>
              <div className="field-hint" style={{ marginTop: 10 }}>
                事件通过 <span className="mono">{"{ evt: 'nfc', type: 'card' }"}</span> 主动推送，无需轮询。
              </div>
            </div>
          </div>
        </div>
      </div>

      <div className="panel log-panel" style={{ marginTop: 18 }}>
        <div className="panel-head">
          <div className="panel-title-row">
            <div className="panel-title">
              <ScrollText size={15} /> 设备日志
            </div>
            <div className="panel-sub">包含设备启动日志、JSON 命令与错误信息</div>
          </div>
          <div className="panel-actions">
            <CopyButton value={filteredLogs.map((entry) => entry.text).join('\n')} label="复制" />
            <Button variant="ghost" size="sm" icon={<Trash2 size={12} />} onClick={clearLogs}>
              清空
            </Button>
          </div>
        </div>
        <div className="log-toolbar">
          <div className="segmented">
            {(['all', 'in', 'out', 'sys', 'error'] as LogFilter[]).map((filter) => (
              <button
                type="button"
                key={filter}
                className={logFilter === filter ? 'active' : ''}
                onClick={() => setLogFilter(filter)}
              >
                {filter === 'all' ? '全部' : filter === 'in' ? '接收' : filter === 'out' ? '发送' : filter === 'sys' ? '系统' : '错误'}
              </button>
            ))}
          </div>
          <span style={{ flex: 1 }} />
          <label className="checkbox">
            <input type="checkbox" checked={autoScroll} onChange={(event) => setAutoScroll(event.target.checked)} />
            自动滚动
          </label>
          <Button variant="ghost" size="sm" icon={<Copy size={12} />} onClick={copyLogs}>
            复制当前
          </Button>
        </div>
        <div className="log-list" ref={logRef}>
          {filteredLogs.length === 0 ? (
            <div style={{ padding: 24, color: '#76848f', textAlign: 'center' }}>暂无日志</div>
          ) : (
            filteredLogs.map((entry) => (
              <div className="log-line" key={entry.id}>
                <span className="log-time">{clockTime(entry.ts)}</span>
                <span className={`log-tag ${entry.dir}`}>{entry.dir.toUpperCase()}</span>
                <span className="log-text">{entry.text}</span>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  )
}

function ChipIcon({ present }: { present: boolean }) {
  return present ? <CheckCircle2 size={26} /> : <Cpu size={26} />
}
