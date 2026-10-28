export type View =
  | 'dashboard'
  | 'reader'
  | 'writer'
  | 'library'
  | 'files'
  | 'device'
  | 'extensions'
  | 'settings'

export interface ViewMeta {
  view: View
  label: string
  eyebrow: string
  title: string
  description: string
}

export const VIEW_META: Record<View, ViewMeta> = {
  dashboard: {
    view: 'dashboard',
    label: '概览',
    eyebrow: 'WORKBENCH',
    title: '让每一张卡片，都有故事。',
    description: '连接设备、读取标签、写入内容。所有能力围绕同一个可扩展协议工作。'
  },
  reader: {
    view: 'reader',
    label: '读卡器',
    eyebrow: 'READER',
    title: '读卡器',
    description: '靠近即读。查看 UID、协议参数、NDEF 记录与原始存储区。'
  },
  writer: {
    view: 'writer',
    label: '写卡',
    eyebrow: 'ENCODER',
    title: '把信息写进卡片',
    description: '文本、链接或自定义 NDEF；写入后自动回读校验。'
  },
  library: {
    view: 'library',
    label: '卡片库',
    eyebrow: 'LIBRARY',
    title: '卡片库',
    description: '本机保存的卡片快照、最近写入与会话记录。'
  },
  files: {
    view: 'files',
    label: '文件',
    eyebrow: 'FILES',
    title: '文件',
    description: '像 Windows 资源管理器一样管理 TF 卡：拖放、粘贴、上传和下载。'
  },
  device: {
    view: 'device',
    label: '设备与连接',
    eyebrow: 'DEVICE',
    title: '设备与连接',
    description: '串口、PN532 模块状态、固件能力与实时日志。'
  },
  extensions: {
    view: 'extensions',
    label: '扩展中心',
    eyebrow: 'EXTENSIONS',
    title: '扩展中心',
    description: '面向后续模块的插槽式设计：传输、协议、能力和界面彼此解耦。'
  },
  settings: {
    view: 'settings',
    label: '设置',
    eyebrow: 'PREFERENCES',
    title: '设置',
    description: '外观、连接、数据与演示模式。'
  }
}

const VIEWS = Object.keys(VIEW_META) as View[]

export function isView(value: string): value is View {
  return (VIEWS as string[]).includes(value)
}

export function viewFromHash(): View {
  const hash = window.location.hash.replace(/^#\/?/, '').trim()
  return isView(hash) ? hash : 'dashboard'
}

export function hrefForView(view: View): string {
  return `#/${view}`
}

export function navigate(view: View): void {
  if (viewFromHash() === view) return
  window.location.hash = `#/${view}`
}
