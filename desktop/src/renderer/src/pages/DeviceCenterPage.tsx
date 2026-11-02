import { useEffect, useRef, useState, type CSSProperties, type ReactNode } from 'react'
import {
  Activity,
  Bluetooth,
  Cable,
  Clock3,
  Cpu,
  HardDrive,
  Loader2,
  MemoryStick,
  MonitorCog,
  Music2,
  Pause,
  Play,
  Radio,
  RefreshCw,
  Sun,
  Volume2,
  Wifi
} from 'lucide-react'
import type { LucideIcon } from 'lucide-react'
import { PageHeader } from '../components/PageHeader'
import { Button, EmptyState, InfoBanner, Pill } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { formatBytes } from '../lib/card'
import type { DevicePlayerStats, DeviceSystemStats } from '@shared/types'

function clampPct(value: number): number {
  if (!Number.isFinite(value)) return 0
  return Math.max(0, Math.min(100, value))
}

function pctWidth(value: number): CSSProperties {
  return { width: `${clampPct(value)}%` }
}

function fmtClock(totalSec: number): string {
  const sec = Math.max(0, Math.floor(totalSec || 0))
  return `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(sec % 60).padStart(2, '0')}`
}

function fmtUptime(totalSec: number): string {
  const sec = Math.max(0, Math.floor(totalSec || 0))
  const days = Math.floor(sec / 86400)
  const hours = Math.floor((sec % 86400) / 3600)
  const minutes = Math.floor((sec % 3600) / 60)
  if (days > 0) return `${days} 天 ${hours} 小时`
  if (hours > 0) return `${hours} 小时 ${minutes} 分`
  if (minutes > 0) return `${minutes} 分 ${sec % 60} 秒`
  return `${sec} 秒`
}

function minutesLabel(minutes: number): string {
  return minutes > 0 ? `${minutes} 分钟` : '关闭'
}

function usedPercent(used: number, total: number): number {
  return total > 0 ? (used / total) * 100 : 0
}

function heapUsed(stats: DeviceSystemStats): number {
  return Math.max(0, stats.ram.heapTotal - stats.ram.heapFree)
}

function psramUsed(stats: DeviceSystemStats): number {
  return Math.max(0, stats.ram.psramTotal - stats.ram.psramFree)
}

function appPartitionSize(stats: DeviceSystemStats): number {
  return stats.rom.appTotal || stats.rom.appSize + stats.rom.appFree
}

const TONE_CLASS = {
  blue: 'is-blue',
  aqua: 'is-aqua',
  coral: 'is-coral',
  yellow: 'is-yellow'
} as const

function Meter({
  label,
  pct,
  detail,
  tone
}: {
  label: string
  pct: number
  detail: string
  tone: keyof typeof TONE_CLASS
}) {
  return (
    <div className={`dc-meter ${TONE_CLASS[tone]}`}>
      <div className="dc-meter-head">
        <span>{label}</span>
        <strong>{clampPct(pct).toFixed(0)}%</strong>
      </div>
      <div className="dc-meter-track">
        <span style={pctWidth(pct)} />
      </div>
      <div className="dc-meter-detail">{detail}</div>
    </div>
  )
}

function InfoTile({ label, value }: { label: string; value: ReactNode }) {
  return (
    <div className="dc-info-tile">
      <div className="dc-info-label">{label}</div>
      <div className="dc-info-value">{value}</div>
    </div>
  )
}

function StoryCover() {
  return (
    <div className="dc-cover" aria-hidden="true">
      <Music2 size={20} />
    </div>
  )
}

function StoryPlayer({
  player,
  volume,
  connected
}: {
  player: DevicePlayerStats
  volume: number
  connected: boolean
}) {
  const pct = player.durSec > 0 ? (player.curSec / player.durSec) * 100 : 0
  const ready = connected && player.hasTrack
  return (
    <div className={`dc-player ${ready ? '' : 'is-empty'}`}>
      <div className="dc-player-head">
        <span className="dc-player-badge">{player.story ? '音频故事' : '音乐播放'}</span>
        <span className={`dc-player-state ${player.playing ? 'is-playing' : ''}`}>
          <span className="dc-player-dot" />
          {player.playing ? '播放中' : ready ? '已暂停' : '待机'}
        </span>
      </div>

      <div className="dc-player-main">
        <StoryCover />
        <div className="dc-player-info">
          <div className="dc-player-title" title={player.title}>
            {ready ? player.title || '未命名内容' : '暂无播放内容'}
          </div>
          <div className="dc-player-sub">
            {ready
              ? `${player.codec || 'MP3'} · ${player.bitrate || '—'} kbps · ${
                  player.sampleRate ? `${(player.sampleRate / 1000).toFixed(1)} kHz` : '—'
                }`
              : '在设备上选择故事或音乐后会自动同步'}
          </div>
          <div className="dc-player-track">
            <span style={pctWidth(pct)} />
          </div>
          <div className="dc-player-time">
            <span>{fmtClock(player.curSec)}</span>
            <span className="dc-player-vol">VOL {String(volume).padStart(2, '0')}</span>
            <span>{fmtClock(player.durSec)}</span>
          </div>
        </div>
        <div className="dc-player-btn" aria-hidden="true">
          {player.playing ? <Pause size={16} /> : <Play size={16} />}
        </div>
      </div>
    </div>
  )
}

function ConfigSlider({
  icon: Icon,
  label,
  hint,
  value,
  min,
  max,
  suffix,
  onChange,
  onCommit
}: {
  icon: LucideIcon
  label: string
  hint: string
  value: number
  min: number
  max: number
  suffix: string
  onChange: (value: number) => void
  onCommit: () => void
}) {
  const fill = max > min ? ((value - min) / (max - min)) * 100 : 0
  return (
    <div className="dc-control">
      <div className="dc-control-head">
        <span className="dc-control-icon">
          <Icon size={16} />
        </span>
        <div className="dc-control-copy">
          <span className="dc-control-label">{label}</span>
          <span className="dc-control-hint">{hint}</span>
        </div>
        <span className="dc-control-value">
          {value}
          <small>{suffix}</small>
        </span>
      </div>
      <input
        className="dc-range"
        type="range"
        min={min}
        max={max}
        value={value}
        aria-label={label}
        style={{ '--dc-fill': `${fill}%` } as CSSProperties}
        onChange={(event) => onChange(Number(event.target.value))}
        onPointerUp={onCommit}
        onPointerCancel={onCommit}
        onBlur={onCommit}
        onKeyUp={onCommit}
      />
      <div className="dc-range-scale">
        <span>{min}</span>
        <span>{max}</span>
      </div>
    </div>
  )
}

function StatusRow({ icon: Icon, label, value, on }: { icon: LucideIcon; label: string; value: string; on: boolean }) {
  return (
    <div className="dc-status-row">
      <span className={`dc-status-icon ${on ? 'on' : ''}`}>
        <Icon size={14} />
      </span>
      <span className="dc-status-label">{label}</span>
      <span className={`dc-status-value ${on ? 'on' : ''}`}>{value}</span>
    </div>
  )
}

export function DeviceCenterPage() {
  const connection = useAppStore((s) => s.connection)
  const stats = useAppStore((s) => s.deviceStats)
  const statsError = useAppStore((s) => s.deviceStatsError)
  const busy = useAppStore((s) => s.busy)
  const connect = useAppStore((s) => s.connect)
  const fetchDeviceStats = useAppStore((s) => s.fetchDeviceStats)
  const updateDeviceConfig = useAppStore((s) => s.updateDeviceConfig)
  const showToast = useAppStore((s) => s.showToast)
  const setView = useAppStore((s) => s.setView)

  const connected = connection.state === 'connected'
  const [volume, setVolume] = useState(0)
  const [brightness, setBrightness] = useState(100)
  const [dragging, setDragging] = useState(false)
  const volumeRef = useRef(0)
  const brightnessRef = useRef(100)

  useEffect(() => {
    if (!connected) return
    void fetchDeviceStats(true)
    const timer = window.setInterval(() => {
      if (document.visibilityState === 'visible') void fetchDeviceStats(true)
    }, 2000)
    return () => window.clearInterval(timer)
  }, [connected, fetchDeviceStats])

  useEffect(() => {
    if (!stats || dragging) return
    volumeRef.current = stats.settings.volume
    brightnessRef.current = stats.settings.brightness
    setVolume(stats.settings.volume)
    setBrightness(stats.settings.brightness)
  }, [stats, dragging])

  function changeVolume(next: number): void {
    volumeRef.current = next
    setVolume(next)
    setDragging(true)
  }

  function changeBrightness(next: number): void {
    brightnessRef.current = next
    setBrightness(next)
    setDragging(true)
  }

  function commitVolume(): void {
    setDragging(false)
    void updateDeviceConfig({ volume: volumeRef.current }, true)
  }

  function commitBrightness(): void {
    setDragging(false)
    void updateDeviceConfig({ brightness: brightnessRef.current }, true)
  }

  async function refresh(): Promise<void> {
    const next = await fetchDeviceStats(false)
    if (next) showToast('success', '设备状态已刷新', `运行 ${fmtUptime(next.uptime)} · CPU ${next.cpu.load}%`)
  }

  const coreCount = stats?.cpu.cores ?? 2
  const cpuLoad = clampPct(stats?.cpu.load ?? 0)
  const heapPct = stats ? usedPercent(heapUsed(stats), stats.ram.heapTotal) : 0
  const psramPct = stats ? usedPercent(psramUsed(stats), stats.ram.psramTotal) : 0
  const flashPct = stats ? usedPercent(stats.rom.appSize, appPartitionSize(stats)) : 0
  const storagePct = stats ? usedPercent(stats.storage.used, stats.storage.total) : 0
  const gaugeStyle = { '--dc-pct': `${cpuLoad}%` } as CSSProperties

  return (
    <div className="page device-center-page">
      <PageHeader
        view="center"
        actions={
          <>
            <Button
              variant="secondary"
              icon={<Cable size={14} />}
              onClick={() => setView('device')}
            >
              串口与日志
            </Button>
            <Button
              variant="primary"
              icon={<RefreshCw size={14} />}
              onClick={() => void refresh()}
              disabled={!connected}
            >
              刷新状态
            </Button>
          </>
        }
      />

      {!connected && (
        <InfoBanner tone={connection.state === 'error' ? 'error' : 'warn'} icon={<MonitorCog size={16} />}>
          {connection.state === 'error'
            ? connection.message ?? '设备连接失败'
            : connection.state === 'connecting' || connection.state === 'scanning'
              ? `正在${connection.state === 'scanning' ? '扫描串口' : '连接设备'}…`
              : '尚未连接 StoryBox。连接后这里会显示 CPU、内存、存储和设备内部配置。'}
        </InfoBanner>
      )}

      {connected && statsError && (
        <InfoBanner tone="warn" icon={<Activity size={16} />}>
          读取系统状态失败：{statsError}
          <span className="dc-banner-hint">
            如果一直出现，请把固件与上位机一起更新到包含 <code>sys · stats</code> 的版本。
          </span>
        </InfoBanner>
      )}

      {!connected ? (
        <div className="dc-empty">
          <EmptyState
            icon={<MonitorCog size={26} />}
            title="还没有连接故事盒"
            description="插好 USB 后扫描串口。没有硬件时，可在设置里打开演示模式直接预览设备中心。"
            actions={
              <Button
                variant="primary"
                icon={<Cable size={14} />}
                loading={busy}
                onClick={() => void connect()}
              >
                扫描并连接
              </Button>
            }
          />
        </div>
      ) : !stats ? (
        <div className="dc-loading">
          <Loader2 size={20} className="spin" />
          <span>{statsError ? '正在重试读取设备状态…' : '正在读取设备状态…'}</span>
        </div>
      ) : (
        <>
          <section className="dc-hero">
            <div className="dc-hero-copy">
              <div className="dc-hero-kicker">
                <span className="dc-live-dot" />
                DEVICE CENTER · LIVE
              </div>
              <div className="dc-hero-title">
                <h2>{stats.settings.name || 'StoryBox'}</h2>
                <Pill tone="aqua" dot>
                  在线
                </Pill>
              </div>
              <p>
                故事盒已稳定运行 {fmtUptime(stats.uptime)}。这里集中展示芯片负载、内存、Flash、TF
                卡和设置内部的配置，数据每 2 秒从设备读取一次。
              </p>
              <div className="dc-chip-row">
                <span className="dc-chip">
                  <Cpu size={13} /> {coreCount} 核 · {stats.cpu.freq} MHz
                </span>
                <span className="dc-chip">
                  <MemoryStick size={13} /> RAM {heapPct.toFixed(0)}%
                </span>
                <span className="dc-chip">
                  <HardDrive size={13} /> 固件 {formatBytes(stats.rom.appSize)}
                </span>
                <span className="dc-chip">
                  <Clock3 size={13} /> {fmtUptime(stats.uptime)}
                </span>
              </div>
            </div>
            <StoryPlayer player={stats.player} volume={stats.settings.volume} connected={connected} />
          </section>

          <div className="dc-stat-grid">
            <div className="dc-stat-card is-cpu">
              <div className="dc-stat-top">
                <span className="dc-stat-label">
                  <Cpu size={12} /> CPU 占用
                </span>
                <span className="dc-stat-pill">{coreCount} 核</span>
              </div>
              <div className="dc-stat-value">
                {stats.cpu.load}
                <small>%</small>
              </div>
              <div className="dc-stat-track">
                <span style={pctWidth(cpuLoad)} />
              </div>
              <div className="dc-stat-meta">
                {stats.cpu.model} · {stats.cpu.freq} MHz
              </div>
            </div>

            <div className="dc-stat-card is-ram">
              <div className="dc-stat-top">
                <span className="dc-stat-label">
                  <MemoryStick size={12} /> RAM 占用
                </span>
                <span className="dc-stat-pill">内部 SRAM</span>
              </div>
              <div className="dc-stat-value">
                {heapPct.toFixed(0)}
                <small>%</small>
              </div>
              <div className="dc-stat-track">
                <span style={pctWidth(heapPct)} />
              </div>
              <div className="dc-stat-meta">
                {formatBytes(heapUsed(stats))} / {formatBytes(stats.ram.heapTotal)}
              </div>
            </div>

            <div className="dc-stat-card is-rom">
              <div className="dc-stat-top">
                <span className="dc-stat-label">
                  <HardDrive size={12} /> ROM / Flash
                </span>
                <span className="dc-stat-pill">APP</span>
              </div>
              <div className="dc-stat-value">
                {flashPct.toFixed(0)}
                <small>%</small>
              </div>
              <div className="dc-stat-track">
                <span style={pctWidth(flashPct)} />
              </div>
              <div className="dc-stat-meta">
                {formatBytes(stats.rom.appSize)} / {formatBytes(appPartitionSize(stats))}
              </div>
            </div>

            <div className="dc-stat-card is-sd">
              <div className="dc-stat-top">
                <span className="dc-stat-label">
                  <HardDrive size={12} /> TF 卡
                </span>
                <span className="dc-stat-pill">{stats.storage.ready ? '已挂载' : '未挂载'}</span>
              </div>
              <div className="dc-stat-value">
                {stats.storage.ready ? storagePct.toFixed(0) : '--'}
                <small>{stats.storage.ready ? '%' : ''}</small>
              </div>
              <div className="dc-stat-track">
                <span style={pctWidth(storagePct)} />
              </div>
              <div className="dc-stat-meta">
                {stats.storage.ready
                  ? `${formatBytes(stats.storage.used)} / ${formatBytes(stats.storage.total)}`
                  : '插入 TF 卡后显示容量'}
              </div>
            </div>
          </div>

          <div className="dc-grid">
            <div className="dc-main">
              <section className="dc-panel">
                <div className="dc-panel-head">
                  <div>
                    <div className="dc-panel-kicker">LIVE TELEMETRY</div>
                    <h3>芯片与运行</h3>
                  </div>
                  <Pill tone="aqua" dot>
                    每 2 秒采样
                  </Pill>
                </div>
                <div className="dc-panel-body">
                  <div className="dc-runtime-top">
                    <div className="dc-gauge" style={gaugeStyle}>
                      <div className="dc-gauge-inner">
                        <strong>{stats.cpu.load}%</strong>
                        <span>主循环占用</span>
                      </div>
                    </div>
                    <div className="dc-cores">
                      {Array.from({ length: Math.max(1, coreCount) }, (_, index) => (
                        <div className="dc-core" key={index}>
                          <div className="dc-core-head">
                            <span>Core {index}</span>
                            <strong>{stats.cpu.freq} MHz</strong>
                          </div>
                          <div className="dc-core-sub">
                            {index === 1 ? 'UI / 主循环 / 文件传输' : '音频解码 / 系统任务'}
                          </div>
                          <div className="dc-core-foot">
                            <span className="dc-core-dot" />
                            调度中
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>

                  <div className="dc-meter-grid">
                    <Meter
                      label="RAM · 堆"
                      pct={heapPct}
                      detail={`${formatBytes(heapUsed(stats))} / ${formatBytes(stats.ram.heapTotal)} · 最低 ${formatBytes(
                        stats.ram.heapMin
                      )}`}
                      tone="blue"
                    />
                    <Meter
                      label="PSRAM"
                      pct={psramPct}
                      detail={`${formatBytes(psramUsed(stats))} / ${formatBytes(stats.ram.psramTotal)}`}
                      tone="aqua"
                    />
                    <Meter
                      label="Flash · APP"
                      pct={flashPct}
                      detail={`${formatBytes(stats.rom.appSize)} / ${formatBytes(appPartitionSize(stats))}`}
                      tone="coral"
                    />
                    <Meter
                      label="TF 卡"
                      pct={stats.storage.ready ? storagePct : 0}
                      detail={
                        stats.storage.ready
                          ? `${formatBytes(stats.storage.free)} 可用`
                          : '未检测到 TF 卡'
                      }
                      tone="yellow"
                    />
                  </div>

                  <div className="dc-info-grid">
                    <InfoTile label="芯片" value={`${stats.cpu.model} · rev ${stats.cpu.revision}`} />
                    <InfoTile label="固件" value={stats.settings.fw} />
                    <InfoTile label="协议 / 角色" value={`v${stats.settings.proto} · ${stats.settings.role}`} />
                    <InfoTile label="SDK" value={stats.cpu.sdk} />
                    <InfoTile label="Flash 芯片" value={formatBytes(stats.rom.flashSize)} />
                    <InfoTile label="应用分区" value={stats.rom.partition} />
                  </div>
                </div>
              </section>

              <section className="dc-panel">
                <div className="dc-panel-head">
                  <div>
                    <div className="dc-panel-kicker">AUDIO &amp; DISPLAY</div>
                    <h3>声音与屏幕</h3>
                  </div>
                  <Pill tone="amber">可调</Pill>
                </div>
                <div className="dc-panel-body">
                  <div className="dc-controls">
                    <ConfigSlider
                      icon={Volume2}
                      label="设备音量"
                      hint={`设备当前音量 · 最大 ${stats.settings.volumeMax} 档`}
                      value={volume}
                      min={0}
                      max={stats.settings.volumeMax}
                      suffix={` / ${stats.settings.volumeMax}`}
                      onChange={changeVolume}
                      onCommit={commitVolume}
                    />
                    <ConfigSlider
                      icon={Sun}
                      label="屏幕亮度"
                      hint={`背光 PWM 亮度 · 最低 ${stats.settings.brightnessMin}%`}
                      value={brightness}
                      min={stats.settings.brightnessMin}
                      max={100}
                      suffix="%"
                      onChange={changeBrightness}
                      onCommit={commitBrightness}
                    />
                  </div>

                  <div className="dc-setting-grid">
                    <InfoTile
                      label="屏幕方向"
                      value={stats.settings.rotation === 1 ? '横屏 0°' : '横屏 180°'}
                    />
                    <InfoTile label="EQ 预设" value={stats.settings.eq} />
                    <InfoTile label="播放熄屏" value={minutesLabel(stats.settings.screenOffMin)} />
                    <InfoTile label="空闲休眠" value={minutesLabel(stats.settings.sleepMin)} />
                    <InfoTile label="童锁" value={stats.settings.childLock ? '已开启' : '关闭'} />
                    <InfoTile label="家长绑定" value={stats.settings.bound ? '已绑定' : '未绑定'} />
                  </div>
                </div>
              </section>
            </div>

            <div className="dc-side">
              <section className="dc-panel">
                <div className="dc-panel-head">
                  <div>
                    <div className="dc-panel-kicker">CONFIG</div>
                    <h3>内部配置</h3>
                  </div>
                  <Pill tone="violet">只读</Pill>
                </div>
                <div className="dc-panel-body">
                  <div className="dc-status-list">
                    <StatusRow
                      icon={Bluetooth}
                      label="蓝牙广播"
                      value={stats.settings.ble ? '开启' : '关闭'}
                      on={stats.settings.ble}
                    />
                    <StatusRow
                      icon={Wifi}
                      label="WiFi STA"
                      value={stats.settings.wifi ? '开启' : '关闭'}
                      on={stats.settings.wifi}
                    />
                    <StatusRow
                      icon={Radio}
                      label="SoftAP"
                      value={stats.settings.ap ? stats.network.apSsid || '开启' : '关闭'}
                      on={stats.settings.ap}
                    />
                  </div>

                  <div className="dc-divider" />

                  <div className="dc-kv-list">
                    <div>
                      <span>设备名称</span>
                      <strong>{stats.settings.name}</strong>
                    </div>
                    <div>
                      <span>固件版本</span>
                      <strong>{stats.settings.fw}</strong>
                    </div>
                    <div>
                      <span>运行时间</span>
                      <strong>{fmtUptime(stats.uptime)}</strong>
                    </div>
                    <div>
                      <span>媒体内容</span>
                      <strong>
                        {stats.storage.tracks} 首音乐 · {stats.storage.stories} 个故事
                      </strong>
                    </div>
                  </div>
                </div>
              </section>

              <section className="dc-panel dc-note-panel">
                <div className="dc-panel-head">
                  <div>
                    <div className="dc-panel-kicker">PLAYBACK</div>
                    <h3>播放状态</h3>
                  </div>
                  <Pill tone="blue">{stats.player.story ? '故事模式' : '音乐模式'}</Pill>
                </div>
                <div className="dc-panel-body">
                  <div className="dc-player-mini">
                    <StoryCover />
                    <div className="dc-player-mini-copy">
                      <strong>{stats.player.hasTrack ? stats.player.title : '暂无播放'}</strong>
                      <span>
                        {stats.player.hasTrack
                          ? `${fmtClock(stats.player.curSec)} / ${fmtClock(stats.player.durSec)} · ${
                              stats.player.playing ? '播放中' : '已暂停'
                            }`
                          : '在设备上选择故事后会自动同步'}
                      </span>
                    </div>
                  </div>
                  <p className="dc-note">
                    播放状态每 2 秒从设备同步，声音与亮度可以在上方调整；故事/音乐播放仍由设备按键或手机控制。
                    其余配置保持只读，避免误改。
                  </p>
                  <Button
                    variant="soft"
                    className="btn-block"
                    icon={<Cable size={14} />}
                    onClick={() => setView('device')}
                  >
                    查看串口与设备日志
                  </Button>
                </div>
              </section>
            </div>
          </div>
        </>
      )}
    </div>
  )
}
