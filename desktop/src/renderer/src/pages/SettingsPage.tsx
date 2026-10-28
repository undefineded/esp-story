import { Download, Monitor, Moon, RotateCcw, Sun, Trash2, Volume2 } from 'lucide-react'
import { PageHeader } from '../components/PageHeader'
import { Button, CopyButton, Pill, Toggle } from '../components/ui'
import { useAppStore } from '../store/useAppStore'
import { bridge } from '../lib/bridge'

export function SettingsPage() {
  const theme = useAppStore((s) => s.theme)
  const setTheme = useAppStore((s) => s.setTheme)
  const settings = useAppStore((s) => s.settings)
  const updateSettings = useAppStore((s) => s.updateSettings)
  const history = useAppStore((s) => s.history)
  const clearHistory = useAppStore((s) => s.clearHistory)
  const demo = useAppStore((s) => s.demo)
  const setDemoMode = useAppStore((s) => s.setDemoMode)
  const showToast = useAppStore((s) => s.showToast)
  const device = useAppStore((s) => s.device)
  const setView = useAppStore((s) => s.setView)

  function exportHistory(): void {
    const blob = new Blob([JSON.stringify({ exportedAt: new Date().toISOString(), cards: history }, null, 2)], {
      type: 'application/json'
    })
    const url = URL.createObjectURL(blob)
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = 'nfc-library.json'
    anchor.click()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
  }

  function resetPreferences(): void {
    if (!window.confirm('恢复外观与连接偏好？卡片库不会被删除。')) return
    localStorage.removeItem('nfc-studio.settings.v1')
    localStorage.removeItem('nfc-studio.theme.v1')
    window.location.reload()
  }

  return (
    <div className="page">
      <PageHeader view="settings" actions={<Pill tone={demo ? 'amber' : 'aqua'} dot>{demo ? '演示会话' : '真实设备会话'}</Pill>} />

      <div className="settings-grid">
        <div className="stack gap-18" style={{ gap: 18 }}>
          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <Monitor size={15} /> 外观
                </div>
                <div className="panel-sub">主题会保存在本机</div>
              </div>
            </div>
            <div className="theme-options">
              <button
                type="button"
                className={`theme-card ${theme === 'light' ? 'active' : ''}`}
                onClick={() => setTheme('light')}
              >
                <span className="theme-swatch light">
                  <span />
                  <span />
                  <span />
                </span>
                <span className="row gap-6">
                  <Sun size={12} /> 浅色
                </span>
              </button>
              <button
                type="button"
                className={`theme-card ${theme === 'dark' ? 'active' : ''}`}
                onClick={() => setTheme('dark')}
              >
                <span className="theme-swatch dark">
                  <span />
                  <span />
                  <span />
                </span>
                <span className="row gap-6">
                  <Moon size={12} /> 深色
                </span>
              </button>
              <button
                type="button"
                className={`theme-card ${theme === 'system' ? 'active' : ''}`}
                onClick={() => setTheme('system')}
              >
                <span className="theme-swatch system">
                  <span />
                  <span />
                  <span />
                </span>
                <span className="row gap-6">
                  <Monitor size={12} /> 跟随系统
                </span>
              </button>
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">连接与交互</div>
                <div className="panel-sub">影响自动握手与提示音</div>
              </div>
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">启动时自动连接</div>
                <div className="settings-row-sub">优先连接上次串口；没有记录时扫描常见 ESP32 端口。</div>
              </div>
              <Toggle
                checked={settings.autoConnect}
                onChange={(next) => void updateSettings({ autoConnect: next })}
                title=""
                ariaLabel="自动连接"
              />
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">卡片提示音</div>
                <div className="settings-row-sub">检测到卡片或写入完成时播放轻提示音。</div>
              </div>
              <Toggle
                checked={settings.sound}
                onChange={(next) => void updateSettings({ sound: next })}
                title=""
                ariaLabel="卡片提示音"
              />
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">波特率</div>
                <div className="settings-row-sub">固件固定 115200，暂不在界面中开放修改。</div>
              </div>
              <span className="pill pill-muted mono">115200</span>
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">数据</div>
                <div className="panel-sub">卡片快照只保存在本机</div>
              </div>
              <Pill tone="muted">{history.length} 张</Pill>
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">保留卡片库</div>
                <div className="settings-row-sub">关闭后仅保留当前会话，不写入本地存储。</div>
              </div>
              <Toggle
                checked={settings.keepHistory}
                onChange={(next) => void updateSettings({ keepHistory: next })}
                title=""
                ariaLabel="保留卡片库"
              />
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">导出卡片库</div>
                <div className="settings-row-sub">JSON 格式，包含 NDEF 记录与原始数据。</div>
              </div>
              <Button variant="secondary" size="sm" icon={<Download size={13} />} onClick={exportHistory} disabled={history.length === 0}>
                导出
              </Button>
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">清空卡片库</div>
                <div className="settings-row-sub">只清除本机快照，不影响卡片本身。</div>
              </div>
              <Button
                variant="secondary"
                size="sm"
                icon={<Trash2 size={13} />}
                onClick={() => {
                  if (history.length === 0) return
                  if (window.confirm('清空本机保存的卡片快照？')) clearHistory()
                }}
                disabled={history.length === 0}
              >
                清空
              </Button>
            </div>
          </div>
        </div>

        <div className="stack gap-18" style={{ gap: 18 }}>
          <div className="about-card">
            <div className="about-brand">
              <div className="about-mark">N</div>
              <div>
                <div className="about-title">StoryBox NFC Studio</div>
                <div className="about-sub">PN532 I2C 桌面工作台</div>
              </div>
              <span style={{ flex: 1 }} />
              <Pill tone="amber">v{bridge.appVersion}</Pill>
            </div>
            <div className="about-grid">
              <div className="about-item">
                <div className="about-item-label">Runtime</div>
                <div className="about-item-value">{bridge.platform === 'demo' ? 'Web Demo' : bridge.platform}</div>
              </div>
              <div className="about-item">
                <div className="about-item-label">Protocol</div>
                <div className="about-item-value">v{device?.proto ?? 2} · JSON lines</div>
              </div>
              <div className="about-item">
                <div className="about-item-label">Board</div>
                <div className="about-item-value">{device?.board ?? 'ESP32-S3'}</div>
              </div>
              <div className="about-item">
                <div className="about-item-label">Reader</div>
                <div className="about-item-value">PN532 · I2C 0x24</div>
              </div>
            </div>
            <div className="about-note">
              协议与硬件接线说明见仓库 <span className="mono">docs/pn532-i2c.md</span>。
              上位机源码位于 <span className="mono">desktop/</span>，使用 Electron + React + TypeScript。
            </div>
          </div>

          <div className="panel">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">
                  <Volume2 size={15} /> 演示与调试
                </div>
                <div className="panel-sub">没有硬件时也能走通全流程</div>
              </div>
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">演示模式</div>
                <div className="settings-row-sub">
                  使用内置模拟设备：自动连接、模拟 NTAG213、写入后立即回显。切换会重新加载页面。
                </div>
              </div>
              <Toggle
                checked={demo}
                onChange={(next) => {
                  setDemoMode(next)
                }}
                title=""
                ariaLabel="演示模式"
              />
            </div>
            <div className="settings-row">
              <div className="settings-row-copy">
                <div className="settings-row-title">验证设备</div>
                <div className="settings-row-sub">重新读取 hello 与模块状态。</div>
              </div>
              <Button variant="secondary" size="sm" onClick={() => setView('device')}>
                打开设备页
              </Button>
            </div>
          </div>

          <div className="panel danger-zone">
            <div className="panel-head">
              <div className="panel-title-row">
                <div className="panel-title">重置</div>
                <div className="panel-sub">只重置界面与连接偏好</div>
              </div>
            </div>
            <div className="panel-body">
              <div className="row gap-12">
                <div className="flex-1 small muted">
                  清除主题、自动连接等本地偏好并重新加载。卡片库与设备固件不受影响。
                </div>
                <Button variant="secondary" size="sm" icon={<RotateCcw size={13} />} onClick={resetPreferences}>
                  恢复默认
                </Button>
              </div>
              <div style={{ marginTop: 14 }}>
                <div className="info-banner" style={{ alignItems: 'center' }}>
                  <span className="flex-1">
                    应用标识 <span className="mono">com.storybox.nfcstudio</span> · 数据目录由 Electron 管理
                  </span>
                  <CopyButton value="com.storybox.nfcstudio" label="复制" />
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
