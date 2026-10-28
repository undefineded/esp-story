import {
  Blocks,
  Cable,
  FolderOpen,
  LayoutDashboard,
  LibraryBig,
  PenLine,
  ScanLine,
  Settings,
  Sparkles
} from 'lucide-react'
import type { LucideIcon } from 'lucide-react'
import { useAppStore } from '../store/useAppStore'
import { bridge } from '../lib/bridge'
import type { View } from '../lib/navigation'

interface NavItem {
  view: View
  label: string
  icon: LucideIcon
  badge?: string
}

const WORKSPACE: NavItem[] = [
  { view: 'dashboard', label: '概览', icon: LayoutDashboard },
  { view: 'reader', label: '读卡器', icon: ScanLine },
  { view: 'writer', label: '写卡', icon: PenLine },
  { view: 'library', label: '卡片库', icon: LibraryBig },
  { view: 'files', label: '文件', icon: FolderOpen }
]

const SYSTEM: NavItem[] = [
  { view: 'device', label: '设备与连接', icon: Cable },
  { view: 'extensions', label: '扩展中心', icon: Blocks, badge: 'BETA' },
  { view: 'settings', label: '设置', icon: Settings }
]

export function Sidebar() {
  const view = useAppStore((s) => s.view)
  const setView = useAppStore((s) => s.setView)
  const connection = useAppStore((s) => s.connection)
  const device = useAppStore((s) => s.device)
  const nfc = useAppStore((s) => s.nfc)
  const demo = useAppStore((s) => s.demo)
  const history = useAppStore((s) => s.history)

  const connected = connection.state === 'connected'
  const moduleOn = connected && nfc.present
  const miniTitle = connected ? device?.name ?? 'StoryBox' : '未连接设备'
  const miniSub = connected
    ? `${connection.port ?? 'USB'} · ${nfc.present ? `PN532 ${nfc.fw}` : '模块未就绪'}`
    : '点击扫描串口'

  function NavGroup({ items }: { items: NavItem[] }) {
    return (
      <div className="nav-list">
        {items.map((item) => {
          const Icon = item.icon
          const badge = item.view === 'library' && history.length > 0 ? String(history.length) : item.badge
          return (
            <button
              key={item.view}
              type="button"
              className={`nav-item ${view === item.view ? 'active' : ''}`}
              onClick={() => setView(item.view)}
              title={item.label}
            >
              <span className="nav-icon">
                <Icon size={17} strokeWidth={1.9} />
              </span>
              <span className="nav-label">{item.label}</span>
              {badge && <span className="nav-badge">{badge}</span>}
            </button>
          )
        })}
      </div>
    )
  }

  return (
    <aside className="sidebar">
      <div className="sidebar-brand">
        <div className="brand-mark" aria-hidden="true" />
        <div className="brand-copy">
          <div className="brand-name">NFC STUDIO</div>
          <div className="brand-sub">小小故事盒 · 桌面工作台</div>
        </div>
      </div>

      <div className="sidebar-section">
        <div className="sidebar-label">Workbench</div>
        <NavGroup items={WORKSPACE} />
      </div>

      <div className="sidebar-section">
        <div className="sidebar-label">System</div>
        <NavGroup items={SYSTEM} />
      </div>

      <div className="sidebar-spacer" />

      <div className="sidebar-foot">
        <button type="button" className="device-mini" onClick={() => setView('device')}>
          <span className={`device-mini-dot ${moduleOn ? 'on' : connected ? 'warn' : ''}`} />
          <span className="device-mini-copy">
            <span className="device-mini-title">{miniTitle}</span>
            <span className="device-mini-sub">{miniSub}</span>
          </span>
        </button>
        <div className="sidebar-version">
          <span>v{bridge.appVersion}</span>
          <span style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
            <Sparkles size={10} /> {demo ? 'DEMO' : 'PN532 · I2C'}
          </span>
        </div>
      </div>
    </aside>
  )
}
