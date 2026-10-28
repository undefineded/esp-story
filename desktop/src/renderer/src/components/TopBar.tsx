import { useEffect, useState } from 'react'
import { Copy, Minus, Moon, RefreshCw, Square, Sun, Wifi, WifiOff, X } from 'lucide-react'
import { useAppStore } from '../store/useAppStore'
import { VIEW_META } from '../lib/navigation'
import { bridge } from '../lib/bridge'
import { IconButton } from './ui'

export function TopBar() {
  const view = useAppStore((s) => s.view)
  const setView = useAppStore((s) => s.setView)
  const connection = useAppStore((s) => s.connection)
  const nfc = useAppStore((s) => s.nfc)
  const demo = useAppStore((s) => s.demo)
  const resolvedTheme = useAppStore((s) => s.resolvedTheme)
  const setTheme = useAppStore((s) => s.setTheme)
  const refresh = useAppStore((s) => s.refresh)
  const scanPorts = useAppStore((s) => s.scanPorts)
  const [maximized, setMaximized] = useState(false)
  // 演示模式使用 MockBridge 处理业务数据，但窗口按钮仍应走 Electron 原生桥。
  const windowControls = window.nfcBridge?.windowControls ?? bridge.windowControls

  useEffect(() => {
    let alive = true
    void windowControls
      .isMaximized()
      .then((value) => {
        if (alive) setMaximized(value)
      })
      .catch(() => undefined)
    const unsubscribe = windowControls.onMaximized(setMaximized)
    return () => {
      alive = false
      unsubscribe()
    }
  }, [windowControls])

  const meta = VIEW_META[view]
  const connected = connection.state === 'connected'

  return (
    <header
      className="topbar"
      onDoubleClick={(event) => {
        if ((event.target as HTMLElement).closest('button, .status-pill, .demo-chip')) return
        void windowControls.toggleMaximize().then(setMaximized)
      }}
    >
      <div className="breadcrumb">
        <span>NFC Studio</span>
        <span className="sep" />
        <strong>{meta.label}</strong>
      </div>

      <div className="topbar-spacer" />

      <div className="topbar-right">
        {demo && <span className="demo-chip">演示模式</span>}

        <div className="status-pill" title={nfc.present ? `PN532 固件 ${nfc.fw}` : '未检测到 PN532'}>
          <span className={`status-dot ${nfc.present ? 'aqua' : connected ? 'warn' : ''}`} />
          <span className="pill-text">{nfc.present ? `PN532 ${nfc.fw}` : '模块离线'}</span>
        </div>

        <button
          type="button"
          className="status-pill clickable"
          onClick={() => setView('device')}
          title="设备与连接"
        >
          <span className={`status-dot ${connected ? 'on' : connection.state === 'error' ? 'err' : 'warn'}`} />
          {connected ? <Wifi size={13} /> : <WifiOff size={13} />}
          <span className="pill-text">
            {connected ? connection.port ?? 'USB 已连接' : connection.state === 'connecting' ? '连接中…' : '未连接'}
          </span>
        </button>

        <span className="topbar-divider" />

        <IconButton
          label={resolvedTheme === 'dark' ? '切换到浅色' : '切换到深色'}
          onClick={() => setTheme(resolvedTheme === 'dark' ? 'light' : 'dark')}
        >
          {resolvedTheme === 'dark' ? <Sun size={16} /> : <Moon size={16} />}
        </IconButton>

        <IconButton
          label="刷新卡片与模块状态"
          onClick={() => {
            if (connected) void refresh()
            else void scanPorts()
          }}
        >
          <RefreshCw size={15} />
        </IconButton>

        <div className="window-controls">
          <button
            type="button"
            className="window-btn"
            title="最小化"
            aria-label="最小化"
            onClick={() => windowControls.minimize()}
          >
            <Minus size={15} />
          </button>
          <button
            type="button"
            className="window-btn"
            title={maximized ? '还原' : '最大化'}
            aria-label={maximized ? '还原' : '最大化'}
            onClick={() => {
              void windowControls.toggleMaximize().then(setMaximized)
            }}
          >
            {maximized ? <Copy size={13} /> : <Square size={13} />}
          </button>
          <button
            type="button"
            className="window-btn close"
            title="关闭"
            aria-label="关闭"
            onClick={() => windowControls.close()}
          >
            <X size={16} />
          </button>
        </div>
      </div>
    </header>
  )
}
