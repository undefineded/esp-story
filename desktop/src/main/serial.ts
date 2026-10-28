import { EventEmitter } from 'node:events'
import { SerialPort } from 'serialport'
import { crc32, transferWindow } from '@shared/transfer'
import type {
  ConnectionStatus,
  DeviceEvent,
  DeviceInfo,
  FsListing,
  FsReadChunk,
  FsReadStart,
  FsWriteStart,
  LogEntry,
  NfcStatus,
  PortInfo,
  RawCard,
  RawReadRequest,
  RawReadResult,
  WriteAccepted
} from '@shared/types'
import type { SettingsStore } from './settings'

type Pending = {
  resolve: (doc: Record<string, unknown>) => void
  reject: (err: Error) => void
  timer: NodeJS.Timeout
}

type SessionEvents = {
  status: [ConnectionStatus]
  device: [DeviceEvent]
  log: [LogEntry]
}

const delay = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms))

const emptyNfc = (): NfcStatus => ({
  present: false,
  chip: 'PN532',
  fw: '--',
  i2c: '0x24',
  polling: false,
  busy: false,
  uptime: 0,
  heap: 0
})

function normalizeNfc(input: unknown): NfcStatus {
  const raw = (input ?? {}) as Partial<NfcStatus>
  return {
    present: Boolean(raw.present),
    chip: typeof raw.chip === 'string' ? raw.chip : 'PN532',
    fw: typeof raw.fw === 'string' ? raw.fw : '--',
    i2c: typeof raw.i2c === 'string' ? raw.i2c : '0x24',
    polling: Boolean(raw.polling),
    busy: Boolean(raw.busy),
    uptime: Number(raw.uptime) || 0,
    heap: Number(raw.heap) || 0,
    card: (raw.card as RawCard | undefined) ?? undefined
  }
}

function normalizeDevice(doc: Record<string, unknown>): DeviceInfo {
  return {
    fw: typeof doc.fw === 'string' ? doc.fw : 'storybox-nfc',
    proto: Number(doc.proto) || 2,
    board: typeof doc.board === 'string' ? doc.board : 'ESP32-S3',
    name: typeof doc.name === 'string' ? doc.name : 'StoryBox',
    role: typeof doc.role === 'string' ? doc.role : 'storybox',
    features: Array.isArray(doc.features) ? (doc.features as string[]) : [],
    nfc: normalizeNfc(doc.nfc)
  }
}

function openPort(path: string, baudRate: number): Promise<SerialPort> {
  return new Promise((resolve, reject) => {
    const port = new SerialPort({ path, baudRate, autoOpen: false, rtscts: false })
    port.open((err) => {
      if (err) reject(err)
      else resolve(port)
    })
  })
}

function writeLine(port: SerialPort, text: string): Promise<void> {
  return new Promise((resolve, reject) => {
    port.write(text, (err) => (err ? reject(err) : resolve()))
  })
}

function isBluetoothPort(port: PortInfo): boolean {
  const text = `${port.manufacturer ?? ''} ${port.friendlyName ?? ''} ${port.path}`.toLowerCase()
  return /bth|bluetooth|蓝牙|rfcomm/.test(text)
}

function attemptsSource(ports: PortInfo[]): PortInfo[] {
  const filtered = ports.filter((port) => !isBluetoothPort(port))
  return filtered.length > 0 ? filtered : ports
}

/**
 * 一个串口会话: 负责握手、请求/响应匹配、事件订阅、心跳和自动重连。
 * 串口上的设备日志以 '[' 开头, 会原样作为 log 事件送给 UI。
 */
export class NfcSession extends EventEmitter<SessionEvents> {
  private port: SerialPort | null = null
  private rx = ''
  private pending = new Map<number, Pending>()
  private nextId = 1
  private status: ConnectionStatus = { state: 'disconnected' }
  private device: DeviceInfo | undefined
  private nfc: NfcStatus = emptyNfc()
  private heartbeatTimer: NodeJS.Timeout | undefined
  private reconnectTimer: NodeJS.Timeout | undefined
  private manual = false
  private closing = false
  private pingFailures = 0
  private logId = 0
  private pollSent = false
  private ready = false
  private fileBusy = 0
  private transferSession = false
  private transferChunk = 4096

  transferWindowSize(): number {
    return this.device?.features.includes('fs.window4.crc32') ? 4 : 1
  }

  async fsSessionBegin(): Promise<void> {
    if (this.transferSession) throw new Error('BUSY: 已有文件传输任务')
    this.transferSession = true
    try {
      if (this.device?.features.includes('fs.session')) {
        await this.request('fs', { act: 'session_begin' }, 10000)
      }
    } catch (err) { this.transferSession = false; throw err }
  }

  async fsSessionEnd(): Promise<void> {
    try {
      if (this.transferSession && this.device?.features.includes('fs.session')) {
        await this.request('fs', { act: 'session_end' }, 10000)
      }
    } finally { this.transferSession = false }
  }

  constructor(private readonly settings: SettingsStore) {
    super()
  }

  getStatus(): ConnectionStatus {
    return this.status
  }

  async listPorts(): Promise<PortInfo[]> {
    return listPorts()
  }

  private candidates(ports: PortInfo[], preferred?: string): PortInfo[] {
    const score = (p: PortInfo): number => {
      const text = `${p.manufacturer ?? ''} ${p.friendlyName ?? ''}`.toLowerCase()
      let value = 0
      if (isBluetoothPort(p)) value -= 100
      if (p.vendorId?.toLowerCase() === '303a') value += 100
      if (p.vendorId?.toLowerCase() === '10c4') value += 70
      if (p.vendorId?.toLowerCase() === '1a86') value += 70
      if (p.vendorId?.toLowerCase() === '0403') value += 60
      if (/espressif|silicon labs|ch340|ch910|ftdi|usb serial/.test(text)) value += 40
      if (/^com\d+$/.test(p.path)) value += 10
      return value
    }
    const sorted = [...ports].sort((a, b) => score(b) - score(a))
    if (preferred) {
      const hit = sorted.find((p) => p.path.toLowerCase() === preferred.toLowerCase())
      if (hit) return [hit, ...sorted.filter((p) => p !== hit)]
    }
    return sorted
  }

  async connect(path?: string): Promise<ConnectionStatus> {
    this.manual = false
    this.clearReconnect()
    await this.closePort(true)

    this.setState({ state: 'scanning', message: '正在查找串口设备…' })
    this.log('sys', '正在扫描串口…')

    let ports: PortInfo[] = []
    try {
      ports = await this.listPorts()
    } catch (err) {
      this.log('error', `枚举串口失败: ${String(err)}`)
    }

    const preferred = path ?? this.settings.get().lastPort
    const candidates = this.candidates(ports, preferred)

    if (path) {
      const exists = candidates.some((p) => p.path.toLowerCase() === path.toLowerCase())
      if (!exists) candidates.unshift({ path })
    }

    if (candidates.length === 0) {
      this.setState({ state: 'error', message: '没有发现可用串口, 请检查 USB 连接' })
      return this.status
    }

    // 只尝试最像开发板的几个口, 避免在蓝牙/虚拟串口上长时间等待
    const attempts = path ? candidates.slice(0, 1) : attemptsSource(candidates).slice(0, 4)
    for (const item of attempts) {
      try {
        await this.openAndHandshake(item.path)
        const settings = await this.settings.set({ lastPort: item.path })
        this.log('sys', `已记住端口 ${item.path} (${settings.baudRate} baud)`)
        return this.status
      } catch (err) {
        this.log('error', `${item.path}: ${err instanceof Error ? err.message : String(err)}`)
        await this.closePort(true)
      }
    }

    this.setState({
      state: 'error',
      message: '未能连接 StoryBox。请确认设备已上电、固件包含 NFC 协议, 并关闭占用串口的其它程序。'
    })
    return this.status
  }

  async autoConnect(): Promise<ConnectionStatus> {
    const settings = await this.settings.load()
    if (settings.lastPort) return this.connect(settings.lastPort)
    try {
      const ports = await this.listPorts()
      const likely = this.candidates(ports).filter((p) => {
        const text = `${p.manufacturer ?? ''} ${p.friendlyName ?? ''}`.toLowerCase()
        return p.vendorId?.toLowerCase() === '303a' || /espressif|silicon labs|ch340|usb serial/.test(text)
      })
      if (likely.length >= 1) return this.connect(likely[0].path)
    } catch {
      /* 忽略枚举失败 */
    }
    return this.status
  }

  private async openAndHandshake(path: string): Promise<void> {
    this.ready = false
    this.pollSent = false
    this.pingFailures = 0
    this.setState({ state: 'connecting', port: path, message: `正在打开 ${path}…` })
    const settings = await this.settings.load()
    this.log('sys', `打开 ${path} @ ${settings.baudRate} baud`)

    const port = await openPort(path, settings.baudRate)
    this.port = port
    this.rx = ''
    this.bindPort(port)
    this.setState({ state: 'connecting', port: path, message: '等待设备握手…' })

    try {
      await this.handshake(port, path)
    } catch (err) {
      if (this.port === port) await this.closePort(true)
      throw err
    }
  }

  private bindPort(port: SerialPort): void {
    port.on('data', (chunk: Buffer | string) => {
      if (this.port !== port) return
      this.onData(typeof chunk === 'string' ? chunk : chunk.toString('utf8'))
    })
    port.on('close', () => {
      if (this.port !== port) return
      this.onClose()
    })
    port.on('error', (err: Error) => {
      if (this.port !== port) return
      this.log('error', `串口错误: ${err.message}`)
    })
  }

  private isConnected(): boolean {
    return this.status.state === 'connected'
  }

  private async handshake(port: SerialPort, path: string): Promise<void> {
    for (let attempt = 0; attempt < 2; attempt++) {
      if (this.port !== port) throw new Error('串口已关闭')
      if (this.isConnected()) return
      try {
        const doc = await this.request('sys', { act: 'hello' }, 1600)
        await this.onDeviceReady(doc, path)
        return
      } catch (err) {
        if (this.isConnected()) return
        if (attempt === 1) {
          throw new Error('设备没有响应 hello, 请确认固件与串口选择正确')
        }
        await delay(300)
      }
    }
  }

  private async onDeviceReady(doc: Record<string, unknown>, path: string): Promise<void> {
    const device = normalizeDevice(doc)
    this.device = device
    this.nfc = device.nfc
    const wasConnected = this.ready
    this.ready = true
    this.setState({ state: 'connected', port: path, device })
    if (!wasConnected) this.log('sys', `设备已连接: ${device.name} · ${device.fw}`)
    this.startHeartbeat()

    if (!this.pollSent) {
      this.pollSent = true
      try {
        const res = await this.request('nfc', { act: 'poll', on: true }, 2500, true)
        this.applyNfc(normalizeNfc((res.data as Record<string, unknown>) ?? {}))
      } catch (err) {
        this.log('error', `启动 NFC 轮询失败: ${err instanceof Error ? err.message : String(err)}`)
      }
    }
  }

  private applyNfc(nfc: NfcStatus): void {
    this.nfc = nfc
    if (this.device) this.device = { ...this.device, nfc }
    if (this.status.state === 'connected' && this.device) {
      this.setState({ ...this.status, device: this.device })
    }
    this.emit('device', {
      type: 'module',
      present: nfc.present,
      fw: nfc.fw,
      chip: nfc.chip,
      i2c: nfc.i2c,
      polling: nfc.polling
    })
  }

  private startHeartbeat(): void {
    this.stopHeartbeat()
    this.heartbeatTimer = setInterval(() => {
      if (this.status.state !== 'connected') return
      // 文件操作（尤其删除大目录/写 SD）会让主循环短暂无暇处理 ping；
      // 这期间跳过心跳，避免误判为设备掉线后主动拆串口。
      if (this.fileBusy > 0) return
      this.request('sys', { act: 'ping' }, 1600, true)
        .then(() => {
          this.pingFailures = 0
        })
        .catch(() => {
          this.pingFailures += 1
          if (this.pingFailures >= 3) {
            this.log('error', '设备心跳超时, 准备重连')
            this.fatal('设备心跳超时')
          }
        })
    }, 5000)
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer)
    this.heartbeatTimer = undefined
  }

  private onData(text: string): void {
    this.rx += text
    if (this.rx.length > 128 * 1024) {
      this.rx = ''
      this.log('error', '串口接收缓冲溢出, 已清空')
      return
    }
    let index = this.rx.indexOf('\n')
    while (index >= 0) {
      const line = this.rx.slice(0, index).replace(/\r$/, '')
      this.rx = this.rx.slice(index + 1)
      this.onLine(line)
      index = this.rx.indexOf('\n')
    }
  }

  private onLine(line: string): void {
    const text = line.trim()
    if (!text) return
    if (!text.startsWith('{')) {
      this.log('in', text)
      return
    }
    let doc: Record<string, unknown>
    try {
      doc = JSON.parse(text) as Record<string, unknown>
    } catch {
      this.log('in', text)
      return
    }
    this.onMessage(doc)
  }

  private onMessage(doc: Record<string, unknown>): void {
    const id = typeof doc.id === 'number' ? doc.id : 0
    if (id && this.pending.has(id)) {
      const pending = this.pending.get(id)
      if (!pending) return
      this.pending.delete(id)
      clearTimeout(pending.timer)
      if (doc.ok === false) {
        const code = typeof doc.err === 'string' ? doc.err : 'ERROR'
        const msg = typeof doc.msg === 'string' ? doc.msg : ''
        pending.reject(new Error(`${code}${msg ? `: ${msg}` : ''}`))
      } else {
        pending.resolve(doc)
      }
      return
    }

    if (doc.evt === 'boot' || doc.evt === 'hello') {
      if (!this.ready && this.port) {
        const path = this.status.port
        if (path) void this.onDeviceReady(doc, path)
      }
      return
    }

    if (doc.evt === 'nfc') this.onNfcEvent(doc)
  }

  private onNfcEvent(doc: Record<string, unknown>): void {
    const type = doc.type
    if (type === 'module') {
      this.nfc = {
        ...this.nfc,
        present: Boolean(doc.present),
        chip: typeof doc.chip === 'string' ? doc.chip : this.nfc.chip,
        fw: typeof doc.fw === 'string' ? doc.fw : this.nfc.fw,
        i2c: typeof doc.i2c === 'string' ? doc.i2c : this.nfc.i2c,
        polling: typeof doc.polling === 'boolean' ? doc.polling : this.nfc.polling
      }
      if (this.device) this.device = { ...this.device, nfc: this.nfc }
      if (this.device && this.status.state === 'connected') {
        this.setState({ ...this.status, device: this.device })
      }
      this.emit('device', {
        type: 'module',
        present: this.nfc.present,
        fw: this.nfc.fw,
        chip: this.nfc.chip,
        i2c: this.nfc.i2c,
        polling: this.nfc.polling
      })
      return
    }
    if (type === 'card' && doc.card) {
      const card = doc.card as RawCard
      this.nfc = { ...this.nfc, card, present: true }
      if (this.device) this.device = { ...this.device, nfc: this.nfc }
      if (this.status.state === 'connected' && this.device) {
        this.setState({ ...this.status, device: this.device })
      }
      this.emit('device', { type: 'card', card })
      return
    }
    if (type === 'removed') {
      const { card: _card, ...rest } = this.nfc
      this.nfc = rest
      if (this.device) this.device = { ...this.device, nfc: this.nfc }
      if (this.status.state === 'connected' && this.device) {
        this.setState({ ...this.status, device: this.device })
      }
      this.emit('device', { type: 'removed' })
      return
    }
    if (type === 'write') {
      this.emit('device', {
        type: 'write',
        phase: (doc.phase as 'start' | 'writing' | 'verifying' | 'done' | 'error') ?? 'writing',
        pct: Number(doc.pct) || 0,
        written: Number(doc.written) || 0,
        ok: Boolean(doc.ok),
        code: typeof doc.code === 'string' ? doc.code : undefined,
        msg: typeof doc.msg === 'string' ? doc.msg : undefined
      })
    }
  }

  private request(
    cmd: string,
    payload: Record<string, unknown> = {},
    timeoutMs = 3000,
    quiet = false
  ): Promise<Record<string, unknown>> {
    if (!this.port?.isOpen) return Promise.reject(new Error('串口未打开'))
    const id = this.nextId++
    const body = { v: 2, id, cmd, ...payload }
    const line = JSON.stringify(body)
    const isFileCommand = cmd === 'fs'
    if (isFileCommand) this.fileBusy += 1
    let released = false
    const release = (): void => {
      if (released) return
      released = true
      if (isFileCommand && this.fileBusy > 0) this.fileBusy -= 1
    }
    return new Promise<Record<string, unknown>>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        release()
        reject(new Error(`命令超时: ${cmd} ${String(payload.act ?? '')}`))
      }, timeoutMs)
      this.pending.set(id, {
        resolve: (doc) => {
          release()
          resolve(doc)
        },
        reject: (err) => {
          release()
          reject(err)
        },
        timer
      })
      if (!quiet) this.log('out', line.length > 700 ? `${line.slice(0, 700)}…` : line)
      writeLine(this.port as SerialPort, `${line}\n`).catch((err: Error) => {
        const item = this.pending.get(id)
        if (item) {
          this.pending.delete(id)
          clearTimeout(timer)
          release()
          reject(err)
        }
      })    })
  }

  async hello(): Promise<DeviceInfo> {
    const doc = await this.request('sys', { act: 'hello' }, 2500)
    const path = this.status.port ?? ''
    await this.onDeviceReady(doc, path)
    return this.device ?? normalizeDevice(doc)
  }

  async nfcStatus(): Promise<NfcStatus> {
    const doc = await this.request('nfc', { act: 'status' }, 2500)
    const nfc = normalizeNfc((doc.data as Record<string, unknown>) ?? {})
    this.applyNfc(nfc)
    return nfc
  }

  async setPolling(on: boolean): Promise<NfcStatus> {
    const doc = await this.request('nfc', { act: 'poll', on }, 2500)
    const nfc = normalizeNfc((doc.data as Record<string, unknown>) ?? {})
    this.applyNfc(nfc)
    return nfc
  }

  async refresh(): Promise<void> {
    await this.request('nfc', { act: 'refresh' }, 3000)
  }

  async readRaw(req: RawReadRequest): Promise<RawReadResult> {
    const doc = await this.request(
      'nfc',
      { act: 'read', kind: req.kind, start: req.start, count: req.count, key: req.key },
      6000
    )
    const data = (doc.data ?? {}) as Record<string, unknown>
    return {
      kind: req.kind,
      start: Number(data.start) || req.start,
      len: Number(data.len) || 0,
      hex: typeof data.hex === 'string' ? data.hex : ''
    }
  }

  async writeNdef(ndef: string, verify = true): Promise<WriteAccepted> {
    const doc = await this.request('nfc', { act: 'write_ndef', ndef, verify }, 5000)
    return { accepted: Boolean(doc.accepted), len: Number(doc.len) || 0 }
  }

  async writeRaw(req: { kind: 'page' | 'block'; start: number; data: string; key?: string }): Promise<WriteAccepted> {
    const doc = await this.request(
      'nfc',
      { act: 'write_raw', kind: req.kind, start: req.start, data: req.data, key: req.key },
      5000
    )
    return { accepted: Boolean(doc.accepted), len: Number(doc.len) || 0 }
  }

  async ping(): Promise<{ uptime: number; heap: number }> {
    const doc = await this.request('sys', { act: 'ping' }, 2000, true)
    return { uptime: Number(doc.uptime) || 0, heap: Number(doc.heap) || 0 }
  }

  /* ---------- TF card file transfer ---------- */

  async fsList(path: string, offset = 0, limit = 64): Promise<FsListing> {
    const doc = await this.request('fs', { act: 'list', path, offset, limit }, 8000)
    const data = (doc.data ?? {}) as Record<string, unknown>
    const rawEntries = Array.isArray(data.entries) ? data.entries : []
    return {
      path: typeof data.path === 'string' ? data.path : path,
      parent: typeof data.parent === 'string' ? data.parent : '',
      entries: rawEntries.map((item) => {
        const entry = (item ?? {}) as Record<string, unknown>
        return {
          name: typeof entry.name === 'string' ? entry.name : '',
          path: typeof entry.path === 'string' ? entry.path : '',
          dir: Boolean(entry.dir),
          size: Number(entry.size) || 0,
          mtime: Number(entry.mtime) || 0
        }
      }),
      total: Number(data.total) || 0,
      offset: Number(data.offset) || 0,
      limit: Number(data.limit) || limit,
      ready: Boolean(data.ready),
      capacity: Number(data.capacity) || 0,
      used: Number(data.used) || 0,
      free: Number(data.free) || 0
    }
  }

  async fsMkdir(path: string): Promise<void> {
    await this.request('fs', { act: 'mkdir', path }, 10000)
  }

  async fsRemove(path: string): Promise<void> {
    await this.request('fs', { act: 'remove', path }, 120000)
  }

  async fsRename(from: string, to: string): Promise<void> {
    await this.request('fs', { act: 'rename', from, to }, 15000)
  }

  async fsReadStart(path: string): Promise<FsReadStart> {
    const doc = await this.request('fs', { act: 'read_begin', path }, 30000)
    const data = (doc.data ?? {}) as Record<string, unknown>
    return {
      xid: Number(data.xid) || 0,
      path: typeof data.path === 'string' ? data.path : path,
      name: typeof data.name === 'string' ? data.name : path.split('/').pop() || 'file',
      size: Number(data.size) || 0,
      chunk: Number(data.chunk) || 4096
    }
  }

  async fsReadChunk(xid: number, offset: number): Promise<FsReadChunk> {
    let doc: Record<string, unknown> | undefined
    let lastError: Error | undefined
    for (let attempt = 0; attempt < 2; attempt++) {
      try {
        doc = await this.request('fs', { act: 'read_chunk', xid, offset }, 30000, true)
        break
      } catch (err) {
        lastError = err instanceof Error ? err : new Error(String(err))
        if (!/超时/.test(lastError.message)) throw lastError
        await delay(250)
      }
    }
    if (!doc) throw lastError ?? new Error('读取超时')
    const data = (doc.data ?? {}) as Record<string, unknown>
    const encoded = typeof data.data === 'string' ? data.data : ''
    const buffer = Buffer.from(encoded, 'base64')
    if (Number(data.offset) !== offset || Number(data.len) !== buffer.length ||
        (this.transferWindowSize() > 1 && typeof data.crc32 !== 'number') ||
        (typeof data.crc32 === 'number' && crc32(buffer) !== data.crc32)) {
      throw new Error('VERIFY: 下载块偏移、长度或校验值不匹配')
    }
    return {
      data: new Uint8Array(buffer),
      offset: Number(data.offset) || offset,
      len: Number(data.len) || buffer.length,
      size: Number(data.size) || 0,
      eof: Boolean(data.eof)
    }
  }

  async fsReadEnd(xid: number): Promise<void> {
    await this.request('fs', { act: 'read_end', xid }, 5000)
  }

  async fsWriteStart(path: string, size: number, overwrite = true): Promise<FsWriteStart> {
    const doc = await this.request('fs', { act: 'write_begin', path, size, overwrite }, 30000)
    const data = (doc.data ?? {}) as Record<string, unknown>
    this.transferChunk = Math.min(4096, Math.max(1, Number(data.chunk) || 4096))
    return {
      xid: Number(data.xid) || 0,
      path: typeof data.path === 'string' ? data.path : path,
      size: Number(data.size) || size,
      chunk: Number(data.chunk) || 4096
    }
  }

  async fsWriteChunk(xid: number, offset: number, data: Uint8Array): Promise<{ written: number }> {
    const payload = Buffer.from(data)
    const chunkSize = this.transferChunk
    const window = this.transferWindowSize()
    let written = offset
    for (let base = 0; base < payload.length; base += chunkSize * window) {
      const jobs: (() => Promise<number>)[] = []
      for (let i = base; i < Math.min(payload.length, base + chunkSize * window); i += chunkSize) {
        jobs.push(async () => {
          const piece = payload.subarray(i, Math.min(i + chunkSize, payload.length))
          let doc: Record<string, unknown> | undefined
          let lastError: Error | undefined
          for (let attempt = 0; attempt < 2; attempt++) {
            try {
              doc = await this.request('fs', {
                act: 'write_chunk', xid, offset: offset + i,
                data: piece.toString('base64'),
                ...(window > 1 ? { crc32: crc32(piece) } : {})
              }, 30000, true)
              break
            } catch (err) {
              lastError = err instanceof Error ? err : new Error(String(err))
              if (!/超时/.test(lastError.message)) throw lastError
              await delay(250)
            }
          }
          if (!doc) throw lastError ?? new Error('写入超时')
          const result = (doc.data ?? {}) as Record<string, unknown>
          const reported = Number(result.written)
          if (!Number.isFinite(reported) || reported < offset + i + piece.length || reported > offset + payload.length) {
            throw new Error('VERIFY: 设备未确认完整的数据块')
          }
          return reported
        })
      }
      written = Math.max(written, ...await transferWindow(jobs))
    }
    return { written }
  }

  async fsWriteEnd(xid: number): Promise<{ size: number; complete: boolean }> {
    const doc = await this.request('fs', { act: 'write_end', xid }, 60000)
    const data = (doc.data ?? {}) as Record<string, unknown>
    if (typeof data.elapsedMs === 'number' && data.elapsedMs > 0) {
      const rate = Number(data.size) * 1000 / data.elapsedMs / 1024
      this.log('sys', `上传完成: ${rate.toFixed(1)} KiB/s; 总耗时 ${data.elapsedMs}ms; SD写入 ${data.diskMs}ms; 最慢写入 ${data.maxDiskMs}ms`)
    }
    return { size: Number(data.size) || 0, complete: Boolean(data.complete) }
  }

  async fsCancel(xid: number): Promise<void> {
    await this.request('fs', { act: 'cancel', xid }, 5000)
  }

  async fsRescan(): Promise<{ tracks: number; categories: number }> {
    const doc = await this.request('fs', { act: 'rescan' }, 120000)
    const data = (doc.data ?? {}) as Record<string, unknown>
    return {
      tracks: Number(data.tracks) || 0,
      categories: Number(data.categories) || 0
    }
  }

  async disconnect(): Promise<void> {
    this.manual = true
    this.clearReconnect()
    await this.closePort(false)
    this.device = undefined
    this.nfc = emptyNfc()
    this.setState({ state: 'disconnected' })
  }

  private async closePort(silent: boolean): Promise<void> {
    this.stopHeartbeat()
    this.ready = false
    this.pollSent = false
    this.rejectPending(new Error('串口已关闭'))
    this.fileBusy = 0
    this.rx = ''
    const port = this.port
    this.port = null
    if (!port) return
    this.closing = true
    if (port.isOpen) {
      await new Promise<void>((resolve) => {
        port.close(() => resolve())
      })
    }
    this.closing = false
    if (!silent) this.log('sys', '串口已关闭')
  }

  private onClose(): void {
    this.transferSession = false
    this.stopHeartbeat()
    this.rejectPending(new Error('设备已断开'))
    this.fileBusy = 0
    this.port = null
    this.ready = false
    this.pollSent = false
    this.device = undefined
    this.nfc = emptyNfc()
    if (this.manual || this.closing) {
      this.setState({ state: 'disconnected' })
      return
    }
    this.setState({ state: 'error', message: '设备连接已断开' })
    this.log('error', '设备连接已断开')
    void this.scheduleReconnect()
  }

  private fatal(message: string): void {
    this.log('error', message)
    this.setState({ state: 'error', message })
    const port = this.port
    if (port?.isOpen) {
      port.close(() => {
        /* onClose 会根据 autoConnect 安排重连 */
      })
    } else {
      this.onClose()
    }
  }

  private async scheduleReconnect(): Promise<void> {
    if (this.manual || this.reconnectTimer) return
    const settings = await this.settings.load()
    if (!settings.autoConnect || !settings.lastPort) return
    this.clearReconnect()
    this.setState({ state: 'connecting', port: settings.lastPort, message: '3 秒后自动重连…' })
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined
      if (this.manual) return
      const port = settings.lastPort
      if (port) void this.connect(port)
    }, 3000)
  }

  private clearReconnect(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer)
    this.reconnectTimer = undefined
  }

  private rejectPending(err: Error): void {
    for (const [, pending] of this.pending) {
      clearTimeout(pending.timer)
      pending.reject(err)
    }
    this.pending.clear()
  }

  private setState(status: ConnectionStatus): void {
    this.status = status
    this.emit('status', status)
  }

  private log(dir: LogEntry['dir'], text: string): void {
    this.emit('log', {
      id: ++this.logId,
      ts: Date.now(),
      dir,
      text: text.slice(0, 4000)
    })
  }
}

export async function listPorts(): Promise<PortInfo[]> {
  const ports = await SerialPort.list()
  return ports
    .map((p) => ({
      path: p.path,
      manufacturer: p.manufacturer,
      friendlyName: p.friendlyName,
      vendorId: p.vendorId,
      productId: p.productId,
      serialNumber: p.serialNumber
    }))
    .sort((a, b) => a.path.localeCompare(b.path, undefined, { numeric: true }))
}
