import {
  ArrowRight,
  Blocks,
  Boxes,
  Cpu,
  DownloadCloud,
  Layers,
  LayoutTemplate,
  PenLine,
  Plus,
  Printer,
  ScanLine,
  ShieldCheck,
  Sparkles,
  Webhook
} from 'lucide-react'
import type { ReactNode } from 'react'
import { PageHeader } from '../components/PageHeader'
import { Button, Pill } from '../components/ui'
import { useAppStore } from '../store/useAppStore'

interface Extension {
  title: string
  description: string
  icon: ReactNode
  tone: 'aqua' | 'coral' | 'amber' | 'violet'
  status: string
  statusTone: 'aqua' | 'amber' | 'muted' | 'violet'
  detail: string
}

const EXTENSIONS: Extension[] = [
  {
    title: '批量写卡',
    description: '把卡片库里的内容排成队列，逐张放入读卡器即可自动写入、失败重试并生成报告。',
    icon: <Layers size={19} />,
    tone: 'aqua',
    status: '规划中',
    statusTone: 'muted',
    detail: '需要固件写入状态机 + 前端队列'
  },
  {
    title: '卡片模板库',
    description: '故事、专辑、网址与自定义 NDEF 模板，带变量和预览，一键填充到写卡表单。',
    icon: <LayoutTemplate size={19} />,
    tone: 'coral',
    status: '基础已具备',
    statusTone: 'aqua',
    detail: '当前版本内置 3 个快捷模板'
  },
  {
    title: '标签打印',
    description: '写入后生成 UID / 内容二维码与贴纸版式，连接标签打印机后批量输出。',
    icon: <Printer size={19} />,
    tone: 'amber',
    status: '规划中',
    statusTone: 'muted',
    detail: '可复用导出 JSON 能力'
  },
  {
    title: 'HTTP / MQTT 联动',
    description: '卡片靠近时触发自动化：播放指定故事、记录听读日志、同步到家庭服务器。',
    icon: <Webhook size={19} />,
    tone: 'violet',
    status: '规划中',
    statusTone: 'muted',
    detail: '事件总线已按类型拆分'
  },
  {
    title: '固件升级',
    description: '通过 USB 串口触发 ESP32-S3 OTA 或进入下载模式，显示版本与升级进度。',
    icon: <DownloadCloud size={19} />,
    tone: 'aqua',
    status: '规划中',
    statusTone: 'muted',
    detail: '依赖 esptool 与分区方案'
  },
  {
    title: '多设备管理',
    description: '同时连接多台故事盒，按工位/教室分组，集中查看模块与卡片状态。',
    icon: <Boxes size={19} />,
    tone: 'coral',
    status: '可扩展',
    statusTone: 'violet',
    detail: '串口会话已按实例封装'
  }
]

export function ExtensionsPage() {
  const device = useAppStore((s) => s.device)
  const setView = useAppStore((s) => s.setView)

  return (
    <div className="page">
      <PageHeader
        view="extensions"
        actions={
          <>
            <Button variant="secondary" icon={<Cpu size={14} />} onClick={() => setView('device')}>
              查看协议
            </Button>
            <Button variant="primary" icon={<Plus size={14} />} onClick={() => setView('writer')}>
              写一张卡
            </Button>
          </>
        }
      />

      <section className="extension-hero">
        <div>
          <div className="page-eyebrow" style={{ marginBottom: 8 }}>
            MODULAR BY DESIGN
          </div>
          <h2>为下一块模块，留好接口。</h2>
          <p>
            上位机没有把 PN532 的逻辑写死在界面里：传输层、JSON 协议、设备能力与页面模块彼此解耦。
            未来接入 WiFi 配网、音频管理或第二块读卡器时，只需要新增一个能力与一个页面。
          </p>
          <div className="row gap-8 wrap" style={{ marginTop: 16 }}>
            <Pill tone="aqua" dot>Transport · USB CDC</Pill>
            <Pill tone="violet" dot>Protocol · JSON v2</Pill>
            <Pill tone="amber" dot>Device · PN532 / NFC</Pill>
            <Pill tone="coral" dot>UI · React modules</Pill>
          </div>
          <div className="row gap-20 wrap" style={{ marginTop: 22 }}>
            <div className="hero-fact">
              <strong>1</strong>
              <span>内置设备模块</span>
            </div>
            <div className="hero-fact">
              <strong>6</strong>
              <span>扩展插槽规划</span>
            </div>
            <div className="hero-fact">
              <strong>v2</strong>
              <span>协议版本</span>
            </div>
          </div>
        </div>
        <div className="architecture">
          <div className="arch-row">
            <div className="arch-node">
              <strong>USB / Serial</strong>
              <span>NfcSession · 自动重连</span>
            </div>
          </div>
          <div className="arch-row">
            <div className="arch-arrow">↓</div>
          </div>
          <div className="arch-row">
            <div className="arch-node">
              <strong>JSON 协议</strong>
              <span>request / response / event</span>
            </div>
          </div>
          <div className="arch-row">
            <div className="arch-arrow">↓</div>
          </div>
          <div className="arch-row">
            <div className="arch-node accent">
              <strong>NFC 能力</strong>
              <span>status · read · write</span>
            </div>
            <div className="arch-node">
              <strong>扩展能力</strong>
              <span>future modules</span>
            </div>
          </div>
          <div className="arch-row">
            <div className="arch-arrow">↓</div>
          </div>
          <div className="arch-row">
            <div className="arch-node">
              <strong>工作台页面</strong>
              <span>reader · writer · device</span>
            </div>
          </div>
        </div>
      </section>

      <div className="extension-grid">
        {EXTENSIONS.map((extension) => (
          <div className="extension-card" key={extension.title}>
            <div className={`extension-icon ${extension.tone}`}>{extension.icon}</div>
            <div className="extension-title">
              {extension.title}
              <Pill tone={extension.statusTone}>{extension.status}</Pill>
            </div>
            <div className="extension-desc">{extension.description}</div>
            <div className="extension-foot">
              <Sparkles size={12} />
              {extension.detail}
            </div>
          </div>
        ))}
      </div>

      <div className="how-grid">
        <div className="how-card">
          <div className="how-step">01</div>
          <div className="how-title">新增设备能力</div>
          <div className="how-desc">
            在固件里新增一个模块，向 HostLink 注册命令与事件；能力通过 hello 的 features 列表暴露。
          </div>
          <div className="how-code">features.add("nfc.raw");
cmd: "nfc" / act: "read"</div>
        </div>
        <div className="how-card">
          <div className="how-step">02</div>
          <div className="how-title">保持协议可演进</div>
          <div className="how-desc">
            每个请求带 id 与版本号，事件用 type 区分；旧客户端忽略未知字段，新客户端按 features 决定是否展示入口。
          </div>
          <div className="how-code">{`{ v: 2, id: 7,
  cmd: "nfc", act: "write_ndef",
  ndef: "D101..." }`}</div>
        </div>
        <div className="how-card">
          <div className="how-step">03</div>
          <div className="how-title">挂载新的界面模块</div>
          <div className="how-desc">
            在 VIEW_META 与侧栏注册一个 View，页面只依赖 store 和 bridge，不直接接触串口或 Electron API。
          </div>
          <div className="how-code">VIEW_META.audio = {`{ label: "音频" }`}
setView("audio")</div>
        </div>
      </div>

      <div className="panel" style={{ marginTop: 18 }}>
        <div className="panel-head">
          <div className="panel-title-row">
            <div className="panel-title">
              <ShieldCheck size={15} /> 当前设备能力
            </div>
            <div className="panel-sub">来自固件 hello 握手，未来会随着模块增加</div>
          </div>
          <Pill tone={device ? 'aqua' : 'muted'} dot>
            {device ? '已连接' : '未连接'}
          </Pill>
        </div>
        <div className="panel-body">
          {device ? (
            <div className="feature-chips">
              {device.features.map((feature) => (
                <Pill tone="aqua" key={feature}>
                  {feature}
                </Pill>
              ))}
              <Pill tone="muted">proto v{device.proto}</Pill>
              <Pill tone="muted">{device.board}</Pill>
            </div>
          ) : (
            <div className="row gap-8">
              <span className="muted small">连接设备后读取能力列表。</span>
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => setView('device')}>
                去连接 <ArrowRight size={12} />
              </button>
            </div>
          )}
          <div className="row gap-8 wrap" style={{ marginTop: 16 }}>
            <Pill tone="coral">
              <ScanLine size={11} /> NFC 读卡
            </Pill>
            <Pill tone="coral">
              <PenLine size={11} /> NDEF 写卡
            </Pill>
            <Pill tone="coral">
              <Blocks size={11} /> 原始页 / 块
            </Pill>
            <Pill tone="coral">
              <ShieldCheck size={11} /> 写入回读校验
            </Pill>
          </div>
        </div>
      </div>
    </div>
  )
}
