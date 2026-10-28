import type {
  AppSettings,
  ConnectionStatus,
  DeviceEvent,
  DeviceInfo,
  FileBridge,
  FsDownloadResult,
  FsListing,
  FsProgress,
  FsReadChunk,
  FsReadStart,
  FsWriteStart,
  LogEntry,
  NfcBridge,
  NfcStatus,
  PortInfo,
  RawCard,
  RawReadRequest,
  RawReadResult,
  WriteAccepted
} from '@shared/types'
import { buildType2UserData, hexToBytes } from './ndef'
import { demoHistoryCards, demoLiveCard } from './demo-data'

const delay = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms))

type MockNode = {
  name: string
  path: string
  dir: boolean
  mtime: number
  size: number
  data?: Uint8Array
  children?: Map<string, MockNode>
}

type MockWrite = {
  path: string
  size: number
  parts: Array<{ offset: number; data: Uint8Array }>
}

function nowSeconds(): number {
  return Math.floor(Date.now() / 1000)
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1)
}

function parentPath(path: string): string {
  const index = path.lastIndexOf('/')
  return index <= 0 ? '/' : path.slice(0, index)
}

function joinPath(dir: string, name: string): string {
  return dir === '/' ? `/${name}` : `${dir}/${name}`
}

const PORTS: PortInfo[] = [
  {
    path: 'COM7',
    manufacturer: 'Espressif Systems',
    friendlyName: 'USB 串行设备 (COM7)',
    vendorId: '303A',
    productId: '1001'
  },
  {
    path: 'COM11',
    manufacturer: 'Silicon Labs',
    friendlyName: 'Silicon Labs CP210x USB to UART Bridge (COM11)',
    vendorId: '10C4',
    productId: 'EA60'
  }
]

function emptyNfc(): NfcStatus {
  return {
    present: false,
    chip: 'PN532',
    fw: '--',
    i2c: '0x24',
    polling: false,
    busy: false,
    uptime: 0,
    heap: 0
  }
}

/** 无硬件时的完整模拟会话, 让界面、写卡流程和动画都可以直接预览 */
export class MockBridge implements NfcBridge {
  isDemo = true
  platform = 'demo'
  appVersion = '1.0.0-demo'

  private status: ConnectionStatus = { state: 'disconnected' }
  private device: DeviceInfo
  private nfc: NfcStatus = emptyNfc()
  private card: RawCard | undefined
  private settings: AppSettings = { autoConnect: true, baudRate: 115200, lastPort: 'COM7' }
  private connListeners = new Set<(status: ConnectionStatus) => void>()
  private eventListeners = new Set<(event: DeviceEvent) => void>()
  private logListeners = new Set<(entry: LogEntry) => void>()
  private logId = 0
  private startedAt = Date.now()
  private writing = false

  private fsRoot: MockNode = {
    name: '',
    path: '/',
    dir: true,
    mtime: nowSeconds(),
    size: 0,
    children: new Map()
  }
  private fsReads = new Map<number, MockNode>()
  private fsWrites = new Map<number, MockWrite>()
  private fsSeq = 100
  private fsProgressListeners = new Set<(progress: FsProgress) => void>()

  files: FileBridge = {
    sessionBegin: async () => {},
    sessionEnd: async () => {},
    list: (path, offset = 0, limit = 64) => this.fsList(path, offset, limit),
    mkdir: (path) => this.fsMkdir(path),
    remove: (path) => this.fsRemove(path),
    rename: (from, to) => this.fsRename(from, to),
    readStart: (path) => this.fsReadStart(path),
    readChunk: (xid, offset) => this.fsReadChunk(xid, offset),
    readEnd: (xid) => this.fsReadEnd(xid),
    writeStart: (path, size, overwrite = true) => this.fsWriteStart(path, size, overwrite),
    writeChunk: (xid, offset, data) => this.fsWriteChunk(xid, offset, data),
    writeEnd: (xid) => this.fsWriteEnd(xid),
    cancel: (xid) => this.fsCancel(xid),
    rescan: () => this.fsRescan(),
    download: (path, defaultName) => this.fsDownload(path, defaultName),
    clipboardPaths: async () => [],
    uploadPaths: async () => ({ files: 0, failed: 0 })
  }

  constructor() {
    this.seedFs()
    this.device = {
      fw: 'storybox-nfc-1.0',
      proto: 4,
      board: 'ESP32-S3',
      name: 'StoryBox-3F2A',
      role: 'storybox',
      features: ['nfc', 'nfc.ndef', 'nfc.raw', 'nfc.write-verify', 'fs', 'fs.read', 'fs.write', 'fs.sd'],
      nfc: this.nfc
    }
  }

  private emitConnection(): void {
    for (const listener of this.connListeners) listener(this.status)
  }

  private emitEvent(event: DeviceEvent): void {
    for (const listener of this.eventListeners) listener(event)
  }

  private log(dir: LogEntry['dir'], text: string): void {
    const entry: LogEntry = { id: ++this.logId, ts: Date.now(), dir, text }
    for (const listener of this.logListeners) listener(entry)
  }

  private setStatus(status: ConnectionStatus): void {
    this.status = status
    this.emitConnection()
  }

  onConnection(cb: (status: ConnectionStatus) => void): () => void {
    this.connListeners.add(cb)
    cb(this.status)
    return () => this.connListeners.delete(cb)
  }

  onDeviceEvent(cb: (event: DeviceEvent) => void): () => void {
    this.eventListeners.add(cb)
    return () => this.eventListeners.delete(cb)
  }

  onLog(cb: (entry: LogEntry) => void): () => void {
    this.logListeners.add(cb)
    return () => this.logListeners.delete(cb)
  }

  onFsProgress(cb: (progress: FsProgress) => void): () => void {
    this.fsProgressListeners.add(cb)
    return () => this.fsProgressListeners.delete(cb)
  }

  windowControls = {
    minimize: (): void => undefined,
    toggleMaximize: async (): Promise<boolean> => false,
    close: (): void => undefined,
    isMaximized: async (): Promise<boolean> => false,
    onMaximized: (_cb: (maximized: boolean) => void): (() => void) => () => undefined
  }

  async listPorts(): Promise<PortInfo[]> {
    await delay(150)
    return PORTS
  }

  async getConnection(): Promise<ConnectionStatus> {
    return this.status
  }

  async connect(path?: string): Promise<ConnectionStatus> {
    const port = path ?? this.settings.lastPort ?? 'COM7'
    this.setStatus({ state: 'scanning', message: '正在查找串口设备…' })
    this.log('sys', '正在扫描串口…')
    await delay(260)
    this.setStatus({ state: 'connecting', port, message: `正在打开 ${port}…` })
    this.log('sys', `打开 ${port} @ 115200 baud`)
    await delay(520)

    this.status = { state: 'connecting', port, message: '等待设备握手…' }
    this.emitConnection()
    this.log('in', '[SYS] PSRAM OK, size: 8388608 bytes')
    this.log('in', '[SYS] FRAM OK')
    this.log('in', '[NFC] PN532 OK, fw 1.6')
    await delay(320)

    this.nfc = {
      ...emptyNfc(),
      present: true,
      fw: '1.6',
      polling: true,
      uptime: 18220,
      heap: 219384,
      ...(this.card ? { card: this.card } : {})
    }
    this.device = { ...this.device, nfc: this.nfc }
    this.settings = { ...this.settings, lastPort: port }
    this.setStatus({ state: 'connected', port, device: this.device })
    this.log('sys', '设备已连接: StoryBox-3F2A · storybox-nfc-1.0')
    this.emitEvent({
      type: 'module',
      present: true,
      fw: '1.6',
      chip: 'PN532',
      i2c: '0x24',
      polling: true
    })

    if (!this.card) {
      setTimeout(() => {
        if (this.status.state === 'connected' && !this.card) this.placeCard()
      }, 900)
    } else {
      this.emitEvent({ type: 'card', card: this.card })
    }
    return this.status
  }

  async disconnect(): Promise<void> {
    this.setStatus({ state: 'disconnected' })
    this.nfc = emptyNfc()
    this.device = { ...this.device, nfc: this.nfc }
    this.log('sys', '串口已关闭')
  }

  async autoConnect(): Promise<ConnectionStatus> {
    return this.connect()
  }

  async hello(): Promise<DeviceInfo> {
    if (this.status.state !== 'connected') await this.connect()
    return this.device
  }

  async nfcStatus(): Promise<NfcStatus> {
    return { ...this.nfc, card: this.card }
  }

  async setPolling(on: boolean): Promise<NfcStatus> {
    this.nfc = { ...this.nfc, polling: on }
    if (this.device) this.device = { ...this.device, nfc: this.nfc }
    if (this.status.state === 'connected') this.setStatus({ ...this.status, device: this.device })
    this.emitEvent({ type: 'module', present: this.nfc.present, fw: this.nfc.fw, polling: on })
    if (on && !this.card) {
      setTimeout(() => {
        if (this.nfc.polling && !this.card) this.placeCard()
      }, 650)
    }
    return this.nfcStatus()
  }

  async refresh(): Promise<void> {
    await delay(180)
    if (this.card) this.emitEvent({ type: 'card', card: this.card })
    else this.placeCard()
  }

  async readRaw(req: RawReadRequest): Promise<RawReadResult> {
    await delay(220)
    if (!this.card) throw new Error('NO_CARD')
    if (req.kind === 'block') {
      const start = req.start
      const bytes = new Uint8Array(req.count * 16)
      const block0 = hexToBytes(this.card.block0 ?? '')
      bytes.set(block0.slice(0, 16), 0)
      for (let i = 16; i < bytes.length; i++) bytes[i] = (i * 7 + 0x5a) & 0xff
      return {
        kind: 'block',
        start,
        len: bytes.length,
        hex: Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
      }
    }
    const start = req.start
    const data = hexToBytes(this.card.data)
    const count = req.count
    const bytes = new Uint8Array(count * 4)
    const header = new Uint8Array(16)
    const uid = hexToBytes(this.card.uid)
    header.set(uid.slice(0, 7), 0)
    const cc = hexToBytes(this.card.cc ?? 'E1101200')
    header.set(cc.slice(0, 4), 12)
    const memory = new Uint8Array(16 + data.length)
    memory.set(header, 0)
    memory.set(data, 16)
    const sourceStart = start * 4
    for (let i = 0; i < bytes.length; i++) {
      const index = sourceStart + i
      bytes[i] = index < memory.length ? memory[index] : (index * 13 + 0x2f) & 0xff
    }
    return {
      kind: 'page',
      start,
      len: bytes.length,
      hex: Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
    }
  }

  async writeNdef(req: { ndef: string; verify?: boolean }): Promise<WriteAccepted> {
    if (this.writing) throw new Error('BUSY')
    const card = this.card
    if (!card) throw new Error('NO_CARD')
    if (!card.writable) throw new Error('READ_ONLY')
    const verify = req.verify !== false
    const message = hexToBytes(req.ndef)
    let userData: Uint8Array
    try {
      userData = buildType2UserData(message, card.cap || 144)
    } catch (err) {
      throw new Error(err instanceof Error ? err.message : 'TOO_LARGE')
    }
    this.writing = true
    this.emitEvent({ type: 'write', phase: 'start', pct: 0, written: 0 })
    try {
      for (let pct = 8; pct < 92; pct += 7) {
        await delay(65)
        this.emitEvent({ type: 'write', phase: 'writing', pct, written: Math.round((pct / 100) * userData.length) })
      }
      await delay(140)
      if (verify) {
        this.emitEvent({ type: 'write', phase: 'verifying', pct: 96, written: userData.length })
        await delay(260)
      }
      const updated: RawCard = {
        ...card,
        formatted: true,
        writable: true,
        cc: `E110${Math.min(0xff, Math.round((card.cap || 144) / 8)).toString(16).padStart(2, '0').toUpperCase()}00`,
        data: Array.from(userData, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
      }
      this.card = updated
      this.nfc = { ...this.nfc, card: updated }
      if (this.device) this.device = { ...this.device, nfc: this.nfc }
      if (this.status.state === 'connected') this.setStatus({ ...this.status, device: this.device })
      this.emitEvent({ type: 'card', card: updated })
      this.emitEvent({ type: 'write', phase: 'done', pct: 100, written: message.length, ok: true })
      this.log('sys', `[NFC] NDEF 写入完成, ${message.length} 字节, 校验通过`)
      return { accepted: true, len: message.length }
    } finally {
      this.writing = false
    }
  }

  async writeRaw(req: { kind: 'page' | 'block'; start: number; data: string; key?: string }): Promise<WriteAccepted> {
    const card = this.card
    if (!card) throw new Error('NO_CARD')
    const patch = hexToBytes(req.data)
    await delay(360)
    if (req.kind === 'page') {
      const data = hexToBytes(card.data)
      const offset = (req.start - 4) * 4
      for (let i = 0; i < patch.length; i++) {
        if (offset + i >= 0 && offset + i < data.length) data[offset + i] = patch[i]
      }
      this.card = {
        ...card,
        data: Array.from(data, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
      }
    } else {
      this.card = {
        ...card,
        block0: Array.from(patch, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase(),
        data: Array.from(patch, (b) => b.toString(16).padStart(2, '0')).join('').toUpperCase()
      }
    }
    this.emitEvent({ type: 'card', card: this.card })
    this.emitEvent({ type: 'write', phase: 'done', pct: 100, written: patch.length, ok: true })
    return { accepted: true, len: patch.length }
  }

  async ping(): Promise<{ uptime: number; heap: number }> {
    return { uptime: Date.now() - this.startedAt, heap: 219384 }
  }

  /* ---------- in-memory TF card ---------- */

  private seedFs(): void {
    const now = nowSeconds()
    this.fsEnsureDir('/music')
    this.fsEnsureDir('/story')
    this.fsEnsureDir('/story/小熊的冬天')
    this.fsEnsureDir('/DCIM')
    this.fsAddFile('/music/月亮邮递员 · 第 03 集.mp3', 4_200_000, undefined, now - 420)
    this.fsAddFile('/music/小星星 · 童谣合辑.mp3', 3_120_000, undefined, now - 8600)
    this.fsAddFile('/music/晚安，小兔子.mp3', 2_680_000, undefined, now - 17200)
    this.fsAddFile('/story/小熊的冬天/01.txt', 0, new TextEncoder().encode('冬天来了，小熊把最后一片枫叶夹进书里。\n'), now - 900)
    this.fsAddFile('/story/小熊的冬天/cover.jpg', 184_320, undefined, now - 900)
    this.fsAddFile('/DCIM/IMG_2031.jpg', 2_840_000, undefined, now - 36400)
    this.fsAddFile('/README.txt', 0, new TextEncoder().encode('StoryBox TF 卡\n\n/music 放音乐\n/story 放故事\n'), now - 120)
    this.fsAddFile('/storybox.json', 0, new TextEncoder().encode('{"name":"StoryBox-3F2A","role":"storybox"}\n'), now - 80)
  }

  private fsFind(path: string): MockNode | undefined {
    if (path === '/') return this.fsRoot
    let node = this.fsRoot
    for (const part of path.split('/').filter(Boolean)) {
      if (!node.dir || !node.children) return undefined
      node = node.children.get(part) as MockNode
      if (!node) return undefined
    }
    return node
  }

  private fsEnsureDir(path: string): MockNode {
    if (path === '/') return this.fsRoot
    let node = this.fsRoot
    let current = ''
    for (const part of path.split('/').filter(Boolean)) {
      current = joinPath(current || '/', part)
      if (!node.children) node.children = new Map()
      let child = node.children.get(part)
      if (!child) {
        child = {
          name: part,
          path: current,
          dir: true,
          mtime: nowSeconds(),
          size: 0,
          children: new Map()
        }
        node.children.set(part, child)
      }
      if (!child.dir) throw new Error('NOT_DIR')
      node = child
    }
    return node
  }

  private fsAddFile(path: string, size: number, data?: Uint8Array, mtime = nowSeconds()): MockNode {
    const parent = this.fsEnsureDir(parentPath(path))
    const name = baseName(path)
    const node: MockNode = { name, path, dir: false, mtime, size: data?.length ?? size, data }
    parent.children?.set(name, node)
    return node
  }

  private fsRemoveNode(node: MockNode): void {
    if (node.dir && node.children) {
      for (const child of node.children.values()) this.fsRemoveNode(child)
      node.children.clear()
    }
    const parent = this.fsFind(parentPath(node.path))
    parent?.children?.delete(node.name)
  }

  private fsUsedBytes(node = this.fsRoot): number {
    if (!node.dir) return node.size
    let total = 0
    for (const child of node.children?.values() ?? []) total += this.fsUsedBytes(child)
    return total
  }

  private fsEmit(progress: FsProgress): void {
    for (const listener of this.fsProgressListeners) listener(progress)
  }

  private async fsList(path: string, offset: number, limit: number): Promise<FsListing> {
    await delay(80)
    const node = this.fsFind(path)
    if (!node) throw new Error('NOT_FOUND')
    if (!node.dir) throw new Error('NOT_DIR')
    const all = Array.from(node.children?.values() ?? []).sort((a, b) => {
      if (a.dir !== b.dir) return a.dir ? -1 : 1
      return a.name.localeCompare(b.name, 'zh-CN')
    })
    const capacity = 32 * 1024 * 1024 * 1024
    const used = this.fsUsedBytes()
    return {
      path,
      parent: parentPath(path),
      entries: all.slice(offset, offset + limit).map((item) => ({
        name: item.name,
        path: item.path,
        dir: item.dir,
        size: item.size,
        mtime: item.mtime
      })),
      total: all.length,
      offset,
      limit,
      ready: true,
      capacity,
      used,
      free: capacity - used
    }
  }

  private async fsMkdir(path: string): Promise<void> {
    await delay(60)
    if (!path || path === '/' || this.fsFind(path)) throw new Error('EXISTS')
    this.fsEnsureDir(path)
  }

  private async fsRemove(path: string): Promise<void> {
    await delay(80)
    if (!path || path === '/') throw new Error('BAD_PATH')
    const node = this.fsFind(path)
    if (!node) throw new Error('NOT_FOUND')
    this.fsRemoveNode(node)
  }

  private async fsRename(from: string, to: string): Promise<void> {
    await delay(80)
    if (!from || !to || from === '/' || to === '/') throw new Error('BAD_PATH')
    const node = this.fsFind(from)
    if (!node) throw new Error('NOT_FOUND')
    if (this.fsFind(to)) throw new Error('EXISTS')
    const parent = this.fsFind(parentPath(from))
    parent?.children?.delete(node.name)
    const nextParent = this.fsEnsureDir(parentPath(to))
    node.name = baseName(to)
    node.path = to
    nextParent.children?.set(node.name, node)
    const update = (item: MockNode): void => {
      if (!item.dir || !item.children) return
      for (const child of item.children.values()) {
        child.path = joinPath(item.path, child.name)
        update(child)
      }
    }
    update(node)
  }

  private async fsReadStart(path: string): Promise<FsReadStart> {
    await delay(60)
    const node = this.fsFind(path)
    if (!node) throw new Error('NOT_FOUND')
    if (node.dir) throw new Error('IS_DIR')
    const xid = ++this.fsSeq
    this.fsReads.set(xid, node)
    return { xid, path, name: node.name, size: node.size, chunk: 4096 }
  }

  private async fsReadChunk(xid: number, offset: number): Promise<FsReadChunk> {
    const node = this.fsReads.get(xid)
    if (!node) throw new Error('BAD_XFER')
    const len = Math.max(0, Math.min(4096, node.size - offset))
    let data: Uint8Array
    if (node.data) {
      data = node.data.slice(offset, offset + len)
    } else {
      data = new Uint8Array(len)
      let seed = 0
      for (let i = 0; i < node.name.length; i++) seed = (seed * 31 + node.name.charCodeAt(i)) & 0xff
      for (let i = 0; i < len; i++) data[i] = (seed + offset + i * 31) & 0xff
    }
    await delay(20)
    return { data, offset, len, size: node.size, eof: offset + len >= node.size }
  }

  private async fsReadEnd(xid: number): Promise<void> {
    this.fsReads.delete(xid)
  }

  private async fsWriteStart(path: string, size: number, overwrite: boolean): Promise<FsWriteStart> {
    if (!path || path === '/') throw new Error('BAD_PATH')
    const existing = this.fsFind(path)
    if (existing?.dir) throw new Error('IS_DIR')
    if (existing && !overwrite) throw new Error('EXISTS')
    const xid = ++this.fsSeq
    this.fsWrites.set(xid, { path, size, parts: [] })
    return { xid, path, size, chunk: 4096 }
  }

  private async fsWriteChunk(xid: number, offset: number, data: Uint8Array): Promise<{ written: number }> {
    const transfer = this.fsWrites.get(xid)
    if (!transfer) throw new Error('BAD_XFER')
    transfer.parts.push({ offset, data: new Uint8Array(data) })
    await delay(12)
    return { written: offset + data.length }
  }

  private async fsWriteEnd(xid: number): Promise<{ size: number; complete: boolean }> {
    const transfer = this.fsWrites.get(xid)
    if (!transfer) throw new Error('BAD_XFER')
    const data = new Uint8Array(transfer.size)
    let written = 0
    for (const part of transfer.parts) {
      data.set(part.data, part.offset)
      written = Math.max(written, part.offset + part.data.length)
    }
    const finalData = data.slice(0, written)
    this.fsAddFile(transfer.path, finalData.length, finalData, nowSeconds())
    this.fsWrites.delete(xid)
    await delay(40)
    return { size: finalData.length, complete: finalData.length === transfer.size }
  }

  private async fsCancel(xid: number): Promise<void> {
    this.fsWrites.delete(xid)
    this.fsReads.delete(xid)
  }

  private async fsRescan(): Promise<{ tracks: number; categories: number }> {
    const music = this.fsFind('/music')
    let tracks = 0
    let categories = 0
    for (const child of music?.children?.values() ?? []) {
      if (child.dir) categories++
      else if (child.name.toLowerCase().endsWith('.mp3')) tracks++
    }
    return { tracks, categories }
  }

  private async fsDownload(path: string, defaultName: string): Promise<FsDownloadResult> {
    const node = this.fsFind(path)
    if (!node) throw new Error('NOT_FOUND')
    if (node.dir) throw new Error('IS_DIR')
    const id = `demo-${Date.now()}`
    this.fsEmit({
      id,
      direction: 'download',
      name: defaultName,
      path,
      written: node.size,
      total: node.size,
      phase: 'done'
    })
    return { canceled: false, path: defaultName, size: node.size }
  }

  async getSettings(): Promise<AppSettings> {
    return this.settings
  }

  async setSettings(patch: Partial<AppSettings>): Promise<AppSettings> {
    this.settings = { ...this.settings, ...patch }
    return this.settings
  }

  private placeCard(overrides?: Partial<RawCard>): void {
    this.card = { ...demoLiveCard(), ...overrides }
    this.nfc = { ...this.nfc, present: true, polling: true, card: this.card }
    if (this.device) this.device = { ...this.device, nfc: this.nfc }
    if (this.status.state === 'connected') this.setStatus({ ...this.status, device: this.device })
    this.emitEvent({ type: 'card', card: this.card })
    this.log('sys', `[NFC] 检测到卡片 ${this.card.type} · ${this.card.uid}`)
  }

  private removeCard(): void {
    this.card = undefined
    const { card: _card, ...rest } = this.nfc
    this.nfc = rest
    if (this.device) this.device = { ...this.device, nfc: this.nfc }
    if (this.status.state === 'connected') this.setStatus({ ...this.status, device: this.device })
    this.emitEvent({ type: 'removed' })
    this.log('sys', '[NFC] 卡片已离开')
  }

  demo = {
    placeCard: (overrides?: Partial<RawCard>): void => this.placeCard(overrides),
    removeCard: (): void => this.removeCard(),
    writeNdef: async (hex: string): Promise<void> => {
      await this.writeNdef({ ndef: hex, verify: true })
    }
  }

  seedHistory() {
    return demoHistoryCards()
  }
}
