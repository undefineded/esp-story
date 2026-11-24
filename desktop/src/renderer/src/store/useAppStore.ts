import { create } from 'zustand'
import type {
  ActivityEntry,
  ConnectionStatus,
  DeviceConfigPatch,
  DeviceEvent,
  DeviceInfo,
  DeviceSystemStats,
  LogEntry,
  MediaCatalogInfo,
  NfcCard,
  NfcStatus,
  PortInfo,
  RawCard,
  RawReadRequest,
  RawReadResult,
  WriteStatus
} from '@shared/types'
import { bridge, isDemo, mockBridge } from '../lib/bridge'
import { toNfcCard } from '../lib/card'
import { buildNdefMessage, bytesToHex, describeNdef, estimateTlvSize } from '../lib/ndef'
import {
  buildResourceUri,
  scanMediaCatalog,
  scanResources,
  type StoryboxResource
} from '../lib/resources'
import { demoHistoryCards } from '../lib/demo-data'
import {
  hasStoredHistory,
  loadHistory,
  loadResources,
  loadTheme,
  loadUiSettings,
  saveHistory,
  saveResources,
  saveTheme,
  saveUiSettings,
  setDemoPreferred,
  type UiSettings
} from '../lib/storage'
import { applyTheme, playChime } from '../lib/theme'
import { navigate, viewFromHash, type View } from '../lib/navigation'

export interface Toast {
  id: number
  tone: 'success' | 'error' | 'info'
  title: string
  message?: string
}

export interface SessionStats {
  cardsRead: number
  writesOk: number
  writesFailed: number
  startedAt: number
}

const emptyNfc = (): NfcStatus => ({
  enabled: true,
  present: false,
  chip: 'PN532',
  fw: '--',
  i2c: '0x24',
  polling: false,
  busy: false,
  uptime: 0,
  heap: 0
})

const idleWrite = (): WriteStatus => ({ phase: 'idle', pct: 0, written: 0, ok: true })
const cachedResources = loadResources()

let toastTimer: ReturnType<typeof setTimeout> | undefined
let activitySeq = 0
let logSeq = 0
let lastCardKey = ''
let lastCardAt = 0
let lastActivityCardAt = 0
let unsubscribers: Array<() => void> = []
let initialized = false
let wasConnected = false

interface AppStore {
  view: View
  theme: 'light' | 'dark' | 'system'
  resolvedTheme: 'light' | 'dark'
  demo: boolean
  initialized: boolean
  connection: ConnectionStatus
  ports: PortInfo[]
  device?: DeviceInfo
  deviceStats?: DeviceSystemStats
  deviceStatsError?: string
  nfc: NfcStatus
  card?: NfcCard
  history: NfcCard[]
  resources: StoryboxResource[]
  resourcesSyncedAt?: number
  resourcesSyncing: boolean
  resourcesError?: string
  activities: ActivityEntry[]
  logs: LogEntry[]
  write: WriteStatus
  settings: UiSettings
  stats: SessionStats
  toast?: Toast
  busy: boolean

  init: () => Promise<void>
  setView: (view: View) => void
  setTheme: (theme: 'light' | 'dark' | 'system') => void
  updateSettings: (patch: Partial<UiSettings>) => Promise<void>
  setDemoMode: (on: boolean) => void
  showToast: (tone: Toast['tone'], title: string, message?: string) => void
  dismissToast: () => void
  addLog: (dir: LogEntry['dir'], text: string) => void
  clearLogs: () => void

  scanPorts: () => Promise<void>
  connect: (path?: string) => Promise<void>
  disconnect: () => Promise<void>
  setPolling: (on: boolean) => Promise<void>
  refresh: () => Promise<void>
  readRaw: (req: RawReadRequest) => Promise<RawReadResult | null>
  syncResources: () => Promise<void>
  writeResource: (resource: StoryboxResource, verify: boolean) => Promise<boolean>
  writeRaw: (kind: 'page' | 'block', start: number, data: string, key?: string) => Promise<boolean>
  clearHistory: () => void
  demoPlaceCard: () => void
  demoRemoveCard: () => void
  pingDevice: () => Promise<{ uptime: number; heap: number } | null>
  fetchDeviceStats: (silent?: boolean) => Promise<DeviceSystemStats | null>
  updateDeviceConfig: (patch: DeviceConfigPatch, quiet?: boolean) => Promise<void>
}

function emptyConnection(): ConnectionStatus {
  return { state: 'disconnected' }
}

export const useAppStore = create<AppStore>((set, get) => {
  function addActivity(
    kind: ActivityEntry['kind'],
    title: string,
    detail?: string,
    cardUid?: string
  ): void {
    const entry: ActivityEntry = {
      id: `act-${++activitySeq}`,
      ts: Date.now(),
      kind,
      title,
      detail,
      cardUid
    }
    set((state) => ({ activities: [entry, ...state.activities].slice(0, 60) }))
  }

  function addLog(dir: LogEntry['dir'], text: string): void {
    const entry: LogEntry = { id: ++logSeq, ts: Date.now(), dir, text: text.slice(0, 4000) }
    set((state) => ({ logs: [...state.logs, entry].slice(-400) }))
  }

  function ingestCard(raw: RawCard, silent = false): void {
    const state = get()
    const previous = state.card?.uid === raw.uid ? state.card : state.history.find((c) => c.uid === raw.uid)
    const card = toNfcCard(raw, previous)
    const history = [card, ...state.history.filter((c) => c.uid !== card.uid)].slice(0, 50)
    const now = Date.now()
    const isNewRead = lastCardKey !== card.uid || now - lastCardAt > 15_000
    lastCardKey = card.uid
    lastCardAt = now

    set({
      card,
      history,
      stats: { ...state.stats, cardsRead: state.stats.cardsRead + (isNewRead ? 1 : 0) }
    })
    if (state.settings.keepHistory) saveHistory(history)

    if (!silent && now - lastActivityCardAt > 1200) {
      lastActivityCardAt = now
      addActivity('card', `读取 ${card.type}`, describeNdef(card.ndef), card.uid)
    }
    if (!silent && state.settings.sound) playChime('card')
  }

  function applyConnection(status: ConnectionStatus): void {
    const state = get()
    const connected = status.state === 'connected'
    if (connected && status.device) {
      const nfc = { ...emptyNfc(), ...status.device.nfc }
      set({ connection: status, device: status.device, nfc, deviceStatsError: undefined })
      if (!wasConnected) {
        wasConnected = true
        addActivity('connect', '设备已连接', `${status.device.name} · ${status.port ?? ''}`)
        addLog('sys', `设备已连接: ${status.device.name} · ${status.device.fw}`)
        if (nfc.card) ingestCard(nfc.card, true)
      }
      return
    }

    if (status.state === 'disconnected' || status.state === 'error') {
      wasConnected = false
      set({
        connection: status,
        device: status.device,
        deviceStats: undefined,
        deviceStatsError: undefined,
        card: undefined,
        nfc: emptyNfc(),
        write: idleWrite()
      })
      if (status.state === 'error' && status.message) addLog('error', status.message)
      return
    }

    set({ connection: status, device: status.device, nfc: status.device?.nfc ?? emptyNfc() })
  }

  function applyDeviceEvent(event: DeviceEvent): void {
    const state = get()
    if (event.type === 'module') {
      const wasPresent = state.nfc.present
      const nfc: NfcStatus = {
        ...state.nfc,
        present: event.present,
        fw: event.fw ?? state.nfc.fw,
        chip: event.chip ?? state.nfc.chip,
        i2c: event.i2c ?? state.nfc.i2c,
        polling: event.polling ?? state.nfc.polling
      }
      set({
        nfc,
        device: state.device ? { ...state.device, nfc } : state.device
      })
      if (!wasPresent && event.present) {
        addActivity('system', 'PN532 模块已就绪', `固件 ${nfc.fw} · I2C ${nfc.i2c}`)
        addLog('sys', `[NFC] PN532 已就绪, 固件 ${nfc.fw}`)
      } else if (wasPresent && !event.present) {
        addActivity('error', 'PN532 模块已断开', '请检查 3.3V、SDA/SCL 与 I2C 模式拨码')
        get().showToast('error', '未检测到 PN532', '检查模块供电与 I2C 接线后重试')
        addLog('error', '[NFC] PN532 模块已断开')
      }
      return
    }

    if (event.type === 'card') {
      ingestCard(event.card)
      return
    }

    if (event.type === 'removed') {
      if (state.card) addActivity('card', '卡片离开', state.card.type, state.card.uid)
      set({ card: undefined })
      return
    }

    if (event.type === 'write') {
      const write: WriteStatus = {
        phase: event.phase,
        pct: event.pct ?? 0,
        written: event.written ?? 0,
        ok: event.ok ?? true,
        code: event.code,
        message: event.msg
      }
      set({ write })
      if (event.phase === 'done') {
        const cardType = state.card?.type ?? '卡片'
        set((s) => ({
          stats: { ...s.stats, writesOk: s.stats.writesOk + 1 }
        }))
        addActivity('write', `写入成功 · ${cardType}`, `${write.written} 字节 · 已回读校验`)
        get().showToast('success', '写入完成', `${write.written} 字节已写入并校验`)
        if (state.settings.sound) playChime('success')
      } else if (event.phase === 'error') {
        set((s) => ({
          stats: { ...s.stats, writesFailed: s.stats.writesFailed + 1 }
        }))
        addActivity('error', '写入失败', event.msg || event.code || '未知错误')
        get().showToast('error', '写入失败', event.msg || event.code || '未知错误')
        if (state.settings.sound) playChime('error')
      }
      return
    }
  }

  function subscribe(): void {
    if (unsubscribers.length > 0) return
    unsubscribers = [
      bridge.onConnection((status) => applyConnection(status)),
      bridge.onDeviceEvent((event) => applyDeviceEvent(event)),
      bridge.onLog((entry) => addLog(entry.dir, entry.text))
    ]
  }

  async function startWrite(ndefHex: string, verify: boolean): Promise<boolean> {
    const state = get()
    if (['start', 'writing', 'verifying'].includes(state.write.phase)) {
      get().showToast('info', '正在写入', '请等待当前写入完成')
      return false
    }
    if (!state.card) {
      get().showToast('error', '还没有卡片', '请先把卡片放到 PN532 天线上')
      return false
    }
    if (!state.card.writable) {
      get().showToast('error', '卡片不可写', '标签可能已锁定或为只读')
      return false
    }
    if (!state.card.type2) {
      get().showToast('error', '这张卡片不支持 NDEF', '资源卡片需要 Type 2 标签')
      return false
    }
    set({ write: { phase: 'start', pct: 0, written: 0, ok: true } })
    try {
      const res = await bridge.writeNdef({ ndef: ndefHex, verify })
      if (!res.accepted) throw new Error('设备拒绝了写入请求')
      // 固件会异步推送进度与结果; 只有仍停在 start 时才推进到 writing,
      // 避免模拟设备已经完成时把状态覆盖回 1%。
      if (get().write.phase === 'start') {
        set({
          write: { phase: 'writing', pct: 1, written: 0, ok: true }
        })
      }
      addLog('sys', `发送 NDEF 写入: ${res.len} 字节, verify=${verify}`)
      return true
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      set({ write: { phase: 'error', pct: 0, written: 0, ok: false, message } })
      get().showToast('error', '写入失败', message)
      addLog('error', `写入失败: ${message}`)
      return false
    }
  }

  return {
    view: 'dashboard',
    theme: 'light',
    resolvedTheme: 'light',
    demo: isDemo,
    initialized: false,
    connection: emptyConnection(),
    ports: [],
    device: undefined,
    deviceStats: undefined,
    deviceStatsError: undefined,
    nfc: emptyNfc(),
    card: undefined,
    history: [],
    resources: cachedResources.items,
    resourcesSyncedAt: cachedResources.syncedAt,
    resourcesSyncing: false,
    resourcesError: undefined,
    activities: [],
    logs: [],
    write: idleWrite(),
    settings: loadUiSettings(),
    stats: { cardsRead: 0, writesOk: 0, writesFailed: 0, startedAt: Date.now() },
    toast: undefined,
    busy: false,

    init: async () => {
      if (initialized) return
      initialized = true
      const theme = loadTheme()
      const resolved = applyTheme(theme)
      const settings = loadUiSettings()
      try {
        const persisted = await bridge.getSettings()
        settings.autoConnect = persisted.autoConnect
      } catch {
        /* 使用本地偏好 */
      }
      let history = loadHistory()
      const initialActivities: ActivityEntry[] = []

      if (isDemo && !hasStoredHistory()) {
        history = demoHistoryCards()
        saveHistory(history)
        initialActivities.push(
          {
            id: `act-${++activitySeq}`,
            ts: Date.now() - 4 * 60_000,
            kind: 'write',
            title: '写入成功 · NTAG213',
            detail: '月亮邮递员 · 第 03 集',
            cardUid: history[0]?.uid
          },
          {
            id: `act-${++activitySeq}`,
            ts: Date.now() - 42 * 60_000,
            kind: 'card',
            title: '读取 NTAG215',
            detail: 'https://storybox.local/album/nursery',
            cardUid: history[1]?.uid
          }
        )
      }

      set({
        theme,
        resolvedTheme: resolved,
        settings,
        history,
        activities: initialActivities,
        stats: { cardsRead: 0, writesOk: 0, writesFailed: 0, startedAt: Date.now() },
        view: viewFromHash()
      })

      subscribe()
      addLog('sys', `NFC Studio ${bridge.appVersion} 已就绪${isDemo ? ' · 演示模式' : ''}`)
      addLog(
        'sys',
        isDemo ? '未检测到 Electron 串口桥, 使用内置模拟设备' : `串口桥已加载 · ${bridge.platform}`
      )

      try {
        const ports = await bridge.listPorts()
        set({ ports })
      } catch (err) {
        addLog('error', `枚举串口失败: ${err instanceof Error ? err.message : String(err)}`)
      }

      try {
        const status = await bridge.getConnection()
        applyConnection(status)
      } catch (err) {
        addLog('error', `读取连接状态失败: ${err instanceof Error ? err.message : String(err)}`)
      }

      if (isDemo) {
        void bridge.autoConnect().catch((err: unknown) => {
          addLog('error', `模拟设备连接失败: ${String(err)}`)
        })
      } else if (settings.autoConnect) {
        setTimeout(() => {
          if (get().connection.state === 'disconnected') {
            void bridge.autoConnect().catch(() => undefined)
          }
        }, 1400)
      }

      window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        if (get().theme === 'system') set({ resolvedTheme: applyTheme('system') })
      })
    },

    setView: (view) => {
      navigate(view)
      set({ view })
    },

    setTheme: (theme) => {
      saveTheme(theme)
      set({ theme, resolvedTheme: applyTheme(theme) })
    },

    updateSettings: async (patch) => {
      const next = { ...get().settings, ...patch }
      set({ settings: next })
      saveUiSettings(next)
      try {
        await bridge.setSettings({ autoConnect: next.autoConnect })
      } catch {
        /* 偏好同步失败不阻断界面 */
      }
    },

    setDemoMode: (on) => {
      setDemoPreferred(on)
      window.location.reload()
    },

    showToast: (tone, title, message) => {
      const toast: Toast = { id: Date.now(), tone, title, message }
      set({ toast })
      if (toastTimer) clearTimeout(toastTimer)
      toastTimer = setTimeout(() => set({ toast: undefined }), 4200)
    },

    dismissToast: () => {
      if (toastTimer) clearTimeout(toastTimer)
      set({ toast: undefined })
    },

    addLog,
    clearLogs: () => set({ logs: [] }),

    scanPorts: async () => {
      try {
        const ports = await bridge.listPorts()
        set({ ports })
        get().showToast('info', '串口列表已刷新', ports.length ? `发现 ${ports.length} 个串口` : '没有发现可用串口')
      } catch (err) {
        get().showToast('error', '刷新失败', err instanceof Error ? err.message : String(err))
      }
    },

    connect: async (path) => {
      set({ busy: true })
      try {
        addLog('sys', path ? `连接串口 ${path}` : '自动扫描并连接设备')
        await bridge.connect(path)
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err)
        get().showToast('error', '连接失败', message)
        addLog('error', `连接失败: ${message}`)
      } finally {
        set({ busy: false })
      }
    },

    disconnect: async () => {
      set({ busy: true })
      try {
        await bridge.disconnect()
      } catch (err) {
        addLog('error', `断开失败: ${String(err)}`)
      } finally {
        set({ busy: false })
      }
    },

    setPolling: async (on) => {
      try {
        const nfc = await bridge.setPolling(on)
        set({ nfc, device: get().device ? { ...get().device!, nfc } : undefined })
      } catch (err) {
        get().showToast('error', '切换轮询失败', err instanceof Error ? err.message : String(err))
      }
    },

    refresh: async () => {
      try {
        await bridge.refresh()
      } catch (err) {
        get().showToast('error', '刷新失败', err instanceof Error ? err.message : String(err))
      }
    },

    readRaw: async (req) => {
      try {
        return await bridge.readRaw(req)
      } catch (err) {
        get().showToast('error', '读取失败', err instanceof Error ? err.message : String(err))
        return null
      }
    },

    syncResources: async () => {
      const state = get()
      if (state.resourcesSyncing) return
      if (state.connection.state !== 'connected') {
        get().showToast('error', '设备未连接', '连接故事机后才能同步 TF 卡内容')
        return
      }
      const features = state.device?.features ?? []
      const mediaSupported = features.includes('media')
      const fsSupported = features.some(
        (feature) => feature === 'fs_transfer' || feature === 'fs_readonly' || feature === 'fs'
      )
      if (!mediaSupported && !fsSupported) {
        get().showToast('error', '固件不支持内容同步', '请升级到带内容索引的固件')
        return
      }
      set({ resourcesSyncing: true, resourcesError: undefined })
      try {
        let items: StoryboxResource[] = []
        let mediaInfo: MediaCatalogInfo | undefined
        let note: string | undefined
        let source = 'fs'
        if (mediaSupported) {
          try {
            mediaInfo = await bridge.media.info()
            items = await scanMediaCatalog(bridge.media, mediaInfo)
            source = 'media'
            if (!mediaInfo.ready) note = '设备尚未建立媒体索引，请先在设备上打开一次音乐/故事列表'
          } catch {
            note = '设备媒体索引不可用，已回退到文件列表'
            items = await scanResources(bridge.files)
          }
        } else {
          items = await scanResources(bridge.files)
        }
        const syncedAt = Date.now()
        set({
          resources: items,
          resourcesSyncedAt: syncedAt,
          resourcesSyncing: false,
          resourcesError: items.length === 0 ? note : undefined
        })
        saveResources(items, syncedAt)
        const music = items.filter((item) => item.kind === 'music').length
        const stories = items.length - music
        if (items.length === 0) {
          get().showToast('info', '内容库为空', note ?? 'TF 卡 /music 与 /story 下没有可用资源')
        } else {
          get().showToast('success', '内容库已同步', `${music} 首音乐 · ${stories} 个故事`)
        }
        addLog(
          'sys',
          `同步内容库(${source}): ${items.length} 项（音乐 ${music} / 故事 ${stories}）${note ? `; ${note}` : ''}`
        )
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err)
        set({ resourcesSyncing: false, resourcesError: message })
        get().showToast('error', '同步内容库失败', message)
        addLog('error', `同步内容库失败: ${message}`)
      }
    },

    writeResource: async (resource, verify) => {
      const state = get()
      if (!state.resources.some((item) => item.id === resource.id)) {
        get().showToast('error', '资源已失效', '请先重新同步 TF 卡内容库')
        return false
      }
      const card = state.card
      if (!card) {
        get().showToast('error', '还没有卡片', '请先把卡片放到 PN532 天线上')
        return false
      }
      if (!card.type2) {
        get().showToast('error', '这张卡片不支持 NDEF', '资源卡片需要 Type 2 标签')
        return false
      }
      if (!card.writable) {
        get().showToast('error', '卡片不可写', '标签可能已锁定或为只读')
        return false
      }
      const message = buildNdefMessage([{ kind: 'uri', uri: buildResourceUri(resource.path) }])
      const needed = estimateTlvSize(message.length)
      if (needed > card.cap) {
        get().showToast('error', '内容太长', `需要 ${needed}B, 当前标签可用 ${card.cap}B`)
        return false
      }
      addLog('sys', `写入资源卡: ${resource.kind} · ${resource.path}`)
      return startWrite(bytesToHex(message), verify)
    },

    writeRaw: async (kind, start, data, key) => {
      const state = get()
      if (['start', 'writing', 'verifying'].includes(state.write.phase)) {
        get().showToast('info', '正在写入', '请等待当前写入完成')
        return false
      }
      try {
        set({ write: { phase: 'start', pct: 0, written: 0, ok: true } })
        await bridge.writeRaw({ kind, start, data, key })
        addLog('sys', `发送原始写入: ${kind} @ ${start}, ${data.length / 2} 字节`)
        return true
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err)
        set({ write: { phase: 'error', pct: 0, written: 0, ok: false, message } })
        get().showToast('error', '原始写入失败', message)
        return false
      }
    },

    clearHistory: () => {
      set({ history: [] })
      saveHistory([])
      get().showToast('info', '卡片库已清空', '本机保存的卡片快照已删除')
    },

    demoPlaceCard: () => {
      mockBridge?.demo?.placeCard()
    },

    demoRemoveCard: () => {
      mockBridge?.demo?.removeCard()
    },

    pingDevice: async () => {
      try {
        return await bridge.ping()
      } catch (err) {
        get().showToast('error', 'Ping 失败', err instanceof Error ? err.message : String(err))
        return null
      }
    },

    fetchDeviceStats: async (silent = true) => {
      try {
        const stats = await bridge.systemStats()
        set({ deviceStats: stats, deviceStatsError: undefined })
        return stats
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err)
        set({ deviceStatsError: message })
        if (!silent) get().showToast('error', '读取设备状态失败', message)
        return null
      }
    },

    updateDeviceConfig: async (patch, quiet = false) => {
      try {
        const stats = await bridge.setSystemConfig(patch)
        set({ deviceStats: stats, deviceStatsError: undefined })
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err)
        set({ deviceStatsError: message })
        if (!quiet) get().showToast('error', '同步设置失败', message)
      }
    }
  }
})
