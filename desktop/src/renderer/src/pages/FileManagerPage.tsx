import {
  AlertCircle,
  ArrowDownUp,
  ArrowLeft,
  ArrowRight,
  ArrowUp,
  Camera,
  Check,
  ChevronRight,
  ClipboardPaste,
  Download,
  File as FileIcon,
  FileArchive,
  FileAudio,
  FileImage,
  FileText,
  Folder,
  FolderOpen,
  FolderPlus,
  HardDrive,
  LayoutGrid,
  List,
  Loader2,
  Minus,
  Move,
  Music2,
  Pencil,
  RefreshCw,
  Search,
  Trash2,
  Upload,
  UploadCloud,
  WifiOff,
  X
} from 'lucide-react'
import type { LucideIcon } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { FsEntry, FsListing, FsProgress } from '@shared/types'
import { bridge, isDemo } from '../lib/bridge'
import { useAppStore } from '../store/useAppStore'
import '../styles/files.css'

type SortKey = 'name' | 'mtime' | 'type' | 'size'
type SortState = { key: SortKey; dir: 1 | -1 }
type ViewMode = 'details' | 'grid'

interface TransferItem {
  id: string
  name: string
  path: string
  direction: 'upload' | 'download'
  written: number
  total: number
  phase: 'start' | 'progress' | 'done' | 'error'
  message?: string
}

interface UploadSource {
  file: File
  relDir: string
}

interface ContextMenuState {
  x: number
  y: number
  entry?: FsEntry
}

interface NameDialog {
  mode: 'new' | 'rename'
  value: string
  entry?: FsEntry
}

interface FileOperation {
  kind: 'move' | 'delete'
  total: number
  current: number
  name: string
  phase: 'running' | 'done' | 'error'
  message?: string
  indeterminate?: boolean
}

const CHUNK = 16 * 1024
const PAGE = 64

const QUICK_LINKS: Array<{ label: string; path: string; icon: LucideIcon }> = [
  { label: 'TF 卡', path: '/', icon: HardDrive },
  { label: '音乐', path: '/music', icon: Music2 },
  { label: '故事', path: '/story', icon: FolderOpen },
  { label: '照片', path: '/DCIM', icon: Camera }
]

function joinPath(dir: string, name: string): string {
  return dir === '/' ? `/${name}` : `${dir}/${name}`
}

function isIndexPath(path: string): boolean {
  return path === '/music' || path.startsWith('/music/') || path === '/story' || path.startsWith('/story/')
}

function parentPath(path: string): string {
  if (path === '/' || !path) return ''
  const index = path.lastIndexOf('/')
  return index <= 0 ? '/' : path.slice(0, index)
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1)
}

function extension(name: string): string {
  const index = name.lastIndexOf('.')
  return index > 0 ? name.slice(index + 1).toLowerCase() : ''
}

function typeLabel(entry: FsEntry): string {
  if (entry.dir) return '文件夹'
  const ext = extension(entry.name)
  const map: Record<string, string> = {
    mp3: 'MP3 音频',
    wav: 'WAV 音频',
    flac: 'FLAC 音频',
    m4a: 'M4A 音频',
    jpg: 'JPEG 图像',
    jpeg: 'JPEG 图像',
    png: 'PNG 图像',
    webp: 'WebP 图像',
    gif: 'GIF 图像',
    txt: '文本文档',
    md: 'Markdown 文档',
    json: 'JSON 文件',
    csv: 'CSV 表格',
    zip: '压缩文件',
    rar: '压缩文件',
    pdf: 'PDF 文档'
  }
  return map[ext] ?? (ext ? `${ext.toUpperCase()} 文件` : '文件')
}

function iconFor(entry: FsEntry): LucideIcon {
  if (entry.dir) return Folder
  const ext = extension(entry.name)
  if (['mp3', 'wav', 'flac', 'm4a', 'aac', 'ogg'].includes(ext)) return FileAudio
  if (['jpg', 'jpeg', 'png', 'webp', 'gif', 'bmp', 'heic'].includes(ext)) return FileImage
  if (['txt', 'md', 'json', 'csv', 'log', 'lrc'].includes(ext)) return FileText
  if (['zip', 'rar', '7z', 'tar', 'gz'].includes(ext)) return FileArchive
  return FileIcon
}

function formatSize(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit += 1
  }
  return `${value >= 10 || unit === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`
}

function formatDate(seconds: number): string {
  if (!seconds) return '—'
  return new Intl.DateTimeFormat('zh-CN', {
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false
  }).format(new Date(seconds * 1000))
}

function friendlyError(err: unknown): string {
  const text = err instanceof Error ? err.message : String(err)
  const code = text.split(':')[0]?.trim()
  const map: Record<string, string> = {
    NO_SD: 'TF 卡未挂载，请插卡后重试',
    NOT_FOUND: '文件或文件夹不存在',
    NOT_DIR: '这不是一个文件夹',
    IS_DIR: '这是一个文件夹',
    EXISTS: '同名文件或文件夹已存在',
    BAD_PATH: '路径不合法',
    IO: 'TF 卡读写失败',
    AUDIO_BUSY: '播放器忙，请停止播放后重试',
    READ_ONLY: 'TF 卡处于只读状态，请检查卡套写保护开关或换一张可写卡',
    NO_SPACE: 'TF 卡空间不足',
    BAD_XFER: '传输已中断，请重试',
    BAD_BASE64: '传输数据损坏',
    RANGE: '传输位置超出文件范围',
    TOO_LARGE: '文件超过 4GB，暂不支持'
  }
  return map[code] ?? text
}

async function readDirectoryEntries(dir: FileSystemDirectoryEntry): Promise<FileSystemEntry[]> {
  const reader = dir.createReader()
  const all: FileSystemEntry[] = []
  for (;;) {
    const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => {
      reader.readEntries(resolve, reject)
    })
    if (batch.length === 0) break
    all.push(...batch)
  }
  return all
}

async function walkEntry(entry: FileSystemEntry, parentRel = ''): Promise<UploadSource[]> {
  if (entry.isFile) {
    const file = await new Promise<File>((resolve, reject) => {
      ;(entry as FileSystemFileEntry).file(resolve, reject)
    })
    return [{ file, relDir: parentRel }]
  }
  if (entry.isDirectory) {
    const dir = entry as FileSystemDirectoryEntry
    const rel = parentRel ? `${parentRel}/${entry.name}` : entry.name
    const children = await readDirectoryEntries(dir)
    const files: UploadSource[] = []
    for (const child of children) files.push(...(await walkEntry(child, rel)))
    return files
  }
  return []
}

async function sourcesFromDataTransfer(data: DataTransfer): Promise<UploadSource[]> {
  const items = Array.from(data.items ?? []).filter((item) => item.kind === 'file')
  const entries = items
    .map((item) => (typeof item.webkitGetAsEntry === 'function' ? item.webkitGetAsEntry() : null))
    .filter((entry): entry is FileSystemEntry => Boolean(entry))
  if (entries.length > 0) {
    const all: UploadSource[] = []
    for (const entry of entries) all.push(...(await walkEntry(entry)))
    return all
  }
  return Array.from(data.files ?? []).map((file) => ({ file, relDir: '' }))
}

function isEditableTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false
  return Boolean(target.closest('input, textarea, select, [contenteditable="true"]'))
}

export function FileManagerPage() {
  const connection = useAppStore((s) => s.connection)
  const device = useAppStore((s) => s.device)
  const demo = useAppStore((s) => s.demo)
  const setView = useAppStore((s) => s.setView)
  const showToast = useAppStore((s) => s.showToast)

  const connected = connection.state === 'connected'
  const supported = demo || Boolean(device?.features?.includes('fs'))
  const fsReady = Boolean(supported && connected)

  const [path, setPath] = useState('/')
  const [listing, setListing] = useState<FsListing | null>(null)
  const [entries, setEntries] = useState<FsEntry[]>([])
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [selected, setSelected] = useState<Set<string>>(new Set())
  const [sort, setSort] = useState<SortState>({ key: 'name', dir: 1 })
  const [view, setViewMode] = useState<ViewMode>('details')
  const [search, setSearch] = useState('')
  const [history, setHistory] = useState<string[]>(['/'])
  const [historyIndex, setHistoryIndex] = useState(0)
  const [transfers, setTransfers] = useState<TransferItem[]>([])
  const [busy, setBusy] = useState(false)
  const [dragging, setDragging] = useState(false)
  const [context, setContext] = useState<ContextMenuState | null>(null)
  const [nameDialog, setNameDialog] = useState<NameDialog | null>(null)
  const [deleteTargets, setDeleteTargets] = useState<FsEntry[] | null>(null)
  const [moveOpen, setMoveOpen] = useState(false)
  const [moveItems, setMoveItems] = useState<FsEntry[]>([])
  const [movePath, setMovePath] = useState('/')
  const [moveDirs, setMoveDirs] = useState<FsEntry[]>([])
  const [moveLoading, setMoveLoading] = useState(false)
  const [operation, setOperation] = useState<FileOperation | null>(null)

  const loadToken = useRef(0)
  const dragDepth = useRef(0)
  const fileInput = useRef<HTMLInputElement>(null)
  const folderInput = useRef<HTMLInputElement>(null)
  const lastClicked = useRef<string | null>(null)
  const pathRef = useRef(path)
  const historyIndexRef = useRef(0)
  const operationTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)
  pathRef.current = path

  useEffect(() => {
    return () => {
      if (operationTimer.current) clearTimeout(operationTimer.current)
    }
  }, [])

  function startOperation(kind: FileOperation['kind'], total: number, name: string, indeterminate = false): void {
    if (operationTimer.current) clearTimeout(operationTimer.current)
    setOperation({ kind, total, current: 0, name, phase: 'running', indeterminate })
  }

  function updateOperation(patch: Partial<FileOperation>): void {
    setOperation((prev) => (prev ? { ...prev, ...patch } : prev))
  }

  function finishOperation(phase: FileOperation['phase'], message?: string): void {
    setOperation((prev) =>
      prev
        ? {
            ...prev,
            phase,
            current: phase === 'done' ? prev.total : prev.current,
            message,
            indeterminate: false
          }
        : prev
    )
    if (operationTimer.current) clearTimeout(operationTimer.current)
    operationTimer.current = setTimeout(() => setOperation(null), phase === 'error' ? 5000 : 3200)
  }

  const loadPath = useCallback(
    async (target: string, push = true) => {
      if (!fsReady) return
      const token = ++loadToken.current
      setLoading(true)
      setError('')
      try {
        let offset = 0
        let total = 0
        let first: FsListing | null = null
        const all: FsEntry[] = []
        for (let page = 0; page < 10; page++) {
          const result = await bridge.files.list(target, offset, PAGE)
          if (token !== loadToken.current) return
          first = result
          total = result.total
          all.push(...result.entries)
          offset += result.entries.length
          if (result.entries.length === 0 || offset >= total) break
        }
        if (token !== loadToken.current) return
        setPath(target)
        setListing(first)
        setEntries(all)
        setSelected(new Set())
        setSearch('')
        if (push) {
          const base = historyIndexRef.current
          setHistory((prev) => {
            const trimmed = prev.slice(0, base + 1)
            if (trimmed[trimmed.length - 1] === target) return trimmed
            return [...trimmed, target]
          })
          historyIndexRef.current = base + 1
          setHistoryIndex(base + 1)
        }
      } catch (err) {
        if (token !== loadToken.current) return
        setEntries([])
        setError(friendlyError(err))
      } finally {
        if (token === loadToken.current) setLoading(false)
      }
    },
    [fsReady]
  )

  const refresh = useCallback(async () => {
    await loadPath(pathRef.current, false)
  }, [loadPath])

  useEffect(() => {
    if (fsReady) void loadPath('/', false)
  }, [fsReady, loadPath])

  useEffect(() => {
    if (!fsReady) return
    return bridge.onFsProgress((progress: FsProgress) => {
      setTransfers((prev) => {
        const id = progress.id
        const existing = prev.find((item) => item.id === id)
        const nextItem: TransferItem = {
          id,
          name: progress.name,
          path: progress.path,
          direction: progress.direction,
          written: progress.written,
          total: progress.total,
          phase: progress.phase,
          message: progress.message
        }
        if (!existing) return [nextItem, ...prev].slice(0, 12)
        return prev.map((item) => (item.id === id ? { ...item, ...nextItem } : item))
      })
    })
  }, [fsReady])

  useEffect(() => {
    if (!context) return
    const close = (): void => setContext(null)
    window.addEventListener('pointerdown', close)
    window.addEventListener('scroll', close, true)
    return () => {
      window.removeEventListener('pointerdown', close)
      window.removeEventListener('scroll', close, true)
    }
  }, [context])

  const sorted = useMemo(() => {
    const needle = search.trim().toLowerCase()
    const filtered = needle
      ? entries.filter((entry) => entry.name.toLowerCase().includes(needle))
      : entries
    return [...filtered].sort((a, b) => {
      if (a.dir !== b.dir) return a.dir ? -1 : 1
      let result = 0
      if (sort.key === 'name') result = a.name.localeCompare(b.name, 'zh-CN')
      else if (sort.key === 'mtime') result = a.mtime - b.mtime
      else if (sort.key === 'size') result = a.size - b.size
      else result = typeLabel(a).localeCompare(typeLabel(b), 'zh-CN')
      return result * sort.dir
    })
  }, [entries, search, sort])

  const selectedEntries = useMemo(
    () => sorted.filter((entry) => selected.has(entry.path)),
    [selected, sorted]
  )
  const allSelected = sorted.length > 0 && sorted.every((entry) => selected.has(entry.path))
  const partiallySelected = selectedEntries.length > 0 && !allSelected

  function toggleSelectAll(): void {
    setSelected(allSelected ? new Set() : new Set(sorted.map((entry) => entry.path)))
  }

  function toggleEntry(entry: FsEntry, event: React.MouseEvent): void {
    event.stopPropagation()
    setSelected((prev) => {
      if (event.shiftKey && lastClicked.current) {
        const from = sorted.findIndex((item) => item.path === lastClicked.current)
        const to = sorted.findIndex((item) => item.path === entry.path)
        if (from >= 0 && to >= 0) {
          const [start, end] = from < to ? [from, to] : [to, from]
          const next = new Set(prev)
          for (const item of sorted.slice(start, end + 1)) next.add(item.path)
          return next
        }
      }
      const next = new Set(prev)
      if (next.has(entry.path)) next.delete(entry.path)
      else next.add(entry.path)
      return next
    })
    lastClicked.current = entry.path
  }

  function toggleSort(key: SortKey): void {
    setSort((prev) => (prev.key === key ? { key, dir: prev.dir === 1 ? -1 : 1 } : { key, dir: 1 }))
  }

  function selectEntry(entry: FsEntry, event: React.MouseEvent): void {
    setContext(null)
    setSelected((prev) => {
      if (event.shiftKey && lastClicked.current) {
        const from = sorted.findIndex((item) => item.path === lastClicked.current)
        const to = sorted.findIndex((item) => item.path === entry.path)
        if (from >= 0 && to >= 0) {
          const [start, end] = from < to ? [from, to] : [to, from]
          return new Set(sorted.slice(start, end + 1).map((item) => item.path))
        }
      }
      if (event.ctrlKey || event.metaKey) {
        const next = new Set(prev)
        if (next.has(entry.path)) next.delete(entry.path)
        else next.add(entry.path)
        return next
      }
      return new Set([entry.path])
    })
    lastClicked.current = entry.path
  }

  function openEntry(entry: FsEntry): void {
    if (entry.dir) {
      void loadPath(entry.path)
      return
    }
    void downloadEntry(entry)
  }

  function goBack(): void {
    if (historyIndex <= 0) return
    const next = historyIndex - 1
    historyIndexRef.current = next
    setHistoryIndex(next)
    void loadPath(history[next], false)
  }

  function goForward(): void {
    if (historyIndex >= history.length - 1) return
    const next = historyIndex + 1
    historyIndexRef.current = next
    setHistoryIndex(next)
    void loadPath(history[next], false)
  }

  function goUp(): void {
    const parent = parentPath(path)
    if (parent) void loadPath(parent)
  }

  function updateTransfer(id: string, patch: Partial<TransferItem>): void {
    setTransfers((prev) => prev.map((item) => (item.id === id ? { ...item, ...patch } : item)))
  }

  async function ensureRemoteDir(target: string, cache: Set<string>, basePath: string): Promise<void> {
    const relative = target.startsWith(basePath) ? target.slice(basePath.length) : target
    const parts = relative.split('/').filter(Boolean)
    let current = basePath
    for (const part of parts) {
      current = joinPath(current, part)
      if (cache.has(current)) continue
      try {
        await bridge.files.mkdir(current)
      } catch (err) {
        if (!/EXISTS/.test(String(err))) throw err
      }
      cache.add(current)
    }
  }

  async function uploadSingle(source: UploadSource, cache: Set<string>, basePath: string): Promise<void> {
    const { file, relDir } = source
    const remoteDir = relDir ? joinPath(basePath, relDir) : basePath
    await ensureRemoteDir(remoteDir, cache, basePath)
    const remotePath = joinPath(remoteDir, file.name)
    const id = `up-${Date.now()}-${Math.random().toString(16).slice(2)}`
    const item: TransferItem = {
      id,
      name: file.name,
      path: remotePath,
      direction: 'upload',
      written: 0,
      total: file.size,
      phase: 'start'
    }
    setTransfers((prev) => [item, ...prev].slice(0, 12))
    let xid = 0
    try {
      const start = await bridge.files.writeStart(remotePath, file.size, true)
      xid = start.xid
      updateTransfer(id, { phase: 'progress' })
      let offset = 0
      while (offset < file.size) {
        const end = Math.min(offset + CHUNK, file.size)
        const chunk = new Uint8Array(await file.slice(offset, end).arrayBuffer())
        await bridge.files.writeChunk(xid, offset, chunk)
        offset = end
        updateTransfer(id, { written: offset, phase: 'progress' })
      }
      const result = await bridge.files.writeEnd(xid)
      updateTransfer(id, {
        written: result.size,
        total: result.size || file.size,
        phase: result.complete ? 'done' : 'error',
        message: result.complete ? undefined : '写入长度不完整'
      })
    } catch (err) {
      if (xid) await bridge.files.cancel(xid).catch(() => undefined)
      updateTransfer(id, {
        phase: 'error',
        message: friendlyError(err)
      })
      throw err
    }
  }

  async function uploadSources(sources: UploadSource[], basePath = pathRef.current): Promise<void> {
    if (!sources.length || busy) return
    setBusy(true)
    const cache = new Set<string>()
    let ok = 0
    let failed = 0
    let touchedIndex = false
    try {
      for (const source of sources) {
        try {
          await uploadSingle(source, cache, basePath)
          ok += 1
          if (isIndexPath(joinPath(basePath, source.relDir)) || isIndexPath(basePath)) {
            touchedIndex = true
          }
        } catch {
          failed += 1
        }
      }
      if (touchedIndex) {
        await bridge.files.rescan().catch(() => undefined)
      }
      await refresh()
      if (failed === 0) {
        showToast('success', '文件已上传', `${ok} 个文件已写入 TF 卡`)
      } else {
        showToast('error', '部分文件上传失败', `${ok} 个成功，${failed} 个失败`)
      }
    } finally {
      setBusy(false)
    }
  }

  async function handleDrop(event: React.DragEvent): Promise<void> {
    event.preventDefault()
    event.stopPropagation()
    dragDepth.current = 0
    setDragging(false)
    if (!fsReady || busy) return

    const internalPaths = readDraggedPaths(event.dataTransfer)
    if (internalPaths.length > 0) {
      await movePaths(internalPaths, pathRef.current)
      return
    }

    try {
      const sources = await sourcesFromDataTransfer(event.dataTransfer)
      if (!sources.length) {
        showToast('info', '没有可上传的文件', '请从 Windows 资源管理器拖入文件或文件夹')
        return
      }
      await uploadSources(sources)
    } catch (err) {
      showToast('error', '读取拖入内容失败', friendlyError(err))
    }
  }

  async function handlePaste(event: ClipboardEvent): Promise<void> {
    if (!fsReady || busy || isEditableTarget(event.target)) return
    if (!event.clipboardData) return
    const sources = await sourcesFromDataTransfer(event.clipboardData).catch(() => [])
    if (sources.length > 0) {
      event.preventDefault()
      await uploadSources(sources)
      return
    }
    // Chromium 对“从资源管理器复制的文件夹”通常不会给出 File 对象，
    // 回退到主进程读取 Windows 剪贴板里的本地路径。
    if (isDemo) {
      showToast('info', '演示模式不支持读取系统剪贴板', '可以从资源管理器拖入文件或文件夹')
      return
    }
    const paths = await bridge.files.clipboardPaths().catch(() => [])
    if (paths.length > 0) {
      event.preventDefault()
      await uploadLocalPaths(paths)
      return
    }
    showToast('info', '剪贴板里没有文件', '先在 Windows 里复制文件或文件夹，再回到这里按 Ctrl+V')
  }

  async function pasteFromClipboard(): Promise<void> {
    if (!fsReady || busy) return
    if (isDemo) {
      showToast('info', '演示模式不支持读取系统剪贴板', '可以从资源管理器拖入文件或文件夹')
      return
    }
    const paths = await bridge.files.clipboardPaths().catch(() => [])
    if (!paths.length) {
      showToast('info', '剪贴板里没有文件', '先在 Windows 里复制文件或文件夹，再点击粘贴')
      return
    }
    await uploadLocalPaths(paths)
  }

  async function uploadLocalPaths(paths: string[]): Promise<void> {
    if (!paths.length || busy) return
    const basePath = pathRef.current
    setBusy(true)
    try {
      const result = await bridge.files.uploadPaths(paths, basePath)
      await refresh()
      if (result.failed === 0) {
        showToast('success', '文件已上传', `${result.files} 个文件已写入 TF 卡`)
      } else {
        showToast('error', '部分文件上传失败', `${result.files} 个成功，${result.failed} 个失败`)
      }
    } catch (err) {
      showToast('error', '粘贴上传失败', friendlyError(err))
    } finally {
      setBusy(false)
    }
  }

  useEffect(() => {
    const handler = (event: ClipboardEvent): void => {
      void handlePaste(event)
    }
    window.addEventListener('paste', handler)
    return () => window.removeEventListener('paste', handler)
  })

  useEffect(() => {
    const onKey = (event: KeyboardEvent): void => {
      if (!fsReady || isEditableTarget(event.target)) return
      if (event.key === 'Backspace') {
        event.preventDefault()
        goUp()
      } else if (event.key === 'F2' && selectedEntries.length === 1) {
        event.preventDefault()
        void renameEntry(selectedEntries[0])
      } else if (event.key === 'Delete' && selectedEntries.length > 0) {
        event.preventDefault()
        void deleteEntries(selectedEntries)
      } else if (event.key === 'Enter' && selectedEntries.length === 1) {
        event.preventDefault()
        openEntry(selectedEntries[0])
      } else if (event.key === 'Escape') {
        setSelected(new Set())
        setContext(null)
      } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a') {
        event.preventDefault()
        setSelected(new Set(sorted.map((entry) => entry.path)))
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  })

  async function downloadEntry(entry: FsEntry): Promise<void> {
    if (entry.dir || busy) return
    setBusy(true)
    try {
      const result = await bridge.files.download(entry.path, entry.name)
      if (result.canceled) return
      if (isDemo) {
        showToast('success', '演示模式：已模拟下载', `${entry.name} · ${formatSize(result.size)}`)
      } else {
        showToast('success', '已下载到本机', result.path ?? entry.name)
      }
    } catch (err) {
      showToast('error', '下载失败', friendlyError(err))
    } finally {
      setBusy(false)
    }
  }

  function readDraggedPaths(data: DataTransfer): string[] {
    const multi = data.getData('application/x-storybox-entries')
    if (multi) {
      try {
        const parsed = JSON.parse(multi) as unknown
        if (Array.isArray(parsed)) return parsed.filter((item): item is string => typeof item === 'string')
      } catch {
        /* fall through */
      }
    }
    const single = data.getData('application/x-storybox-entry')
    return single ? [single] : []
  }

  async function movePaths(paths: string[], destination: string): Promise<void> {
    const targets = paths
      .map((path) => entries.find((entry) => entry.path === path))
      .filter((entry): entry is FsEntry => Boolean(entry))
    await moveEntries(targets, destination)
  }

  async function moveEntries(targets: FsEntry[], destination: string): Promise<void> {
    if (!targets.length || !destination || busy) return
    const unique = Array.from(new Map(targets.map((entry) => [entry.path, entry])).values())
    setBusy(true)
    setMoveOpen(false)
    startOperation('move', unique.length, unique[0]?.name ?? '', Boolean(unique[0]?.dir))
    let moved = 0
    let failed = 0
    let skipped = 0
    let touchedIndex = false
    try {
      for (let index = 0; index < unique.length; index++) {
        const entry = unique[index]
        updateOperation({ current: index, name: entry.name, indeterminate: entry.dir })
        if (parentPath(entry.path) === destination) {
          skipped += 1
          continue
        }
        if (destination === entry.path || destination.startsWith(`${entry.path}/`)) {
          failed += 1
          continue
        }
        const target = joinPath(destination, entry.name)
        try {
          await bridge.files.rename(entry.path, target)
          moved += 1
          if (isIndexPath(entry.path) || isIndexPath(target)) touchedIndex = true
        } catch {
          failed += 1
        }
      }
      updateOperation({ current: unique.length })
      if (touchedIndex) await bridge.files.rescan().catch(() => undefined)
      await refresh()
      if (failed > 0) {
        finishOperation('error', `${moved} 个成功，${failed} 个失败`)
        showToast('error', '部分项目移动失败', `${moved} 个成功，${failed} 个失败`)
      } else if (moved > 0) {
        finishOperation('done')
        showToast('success', '已移动', `${moved} 个项目 → ${destination}`)
      } else if (skipped > 0) {
        finishOperation('done')
        showToast('info', '无需移动', '选中的项目已经在这个文件夹里')
      } else {
        finishOperation('done')
      }
    } catch (err) {
      finishOperation('error', friendlyError(err))
      showToast('error', '移动失败', friendlyError(err))
    } finally {
      setBusy(false)
    }
  }

  async function loadMoveDirs(target: string): Promise<void> {
    setMoveLoading(true)
    try {
      const result = await bridge.files.list(target, 0, 96)
      setMoveDirs(result.entries.filter((entry) => entry.dir))
      setMovePath(target)
    } catch (err) {
      showToast('error', '读取文件夹失败', friendlyError(err))
    } finally {
      setMoveLoading(false)
    }
  }

  function openMove(targets = selectedEntries): void {
    if (!targets.length || busy) return
    setMoveItems(targets)
    setMovePath(pathRef.current)
    setMoveDirs([])
    setMoveOpen(true)
    void loadMoveDirs(pathRef.current)
  }

  function deleteEntries(targets: FsEntry[]): void {
    if (!targets.length || busy) return
    setDeleteTargets(targets)
  }

  async function confirmDelete(): Promise<void> {
    const targets = deleteTargets
    if (!targets || !targets.length || busy) return
    const label = targets.length === 1 ? targets[0].name : `${targets.length} 个项目`
    setBusy(true)
    setDeleteTargets(null)
    startOperation('delete', targets.length, targets[0]?.name ?? '', Boolean(targets[0]?.dir))
    let touchedIndex = false
    try {
      for (let index = 0; index < targets.length; index++) {
        const entry = targets[index]
        updateOperation({ current: index, name: entry.name, indeterminate: entry.dir })
        await bridge.files.remove(entry.path)
        if (isIndexPath(entry.path)) touchedIndex = true
      }
      updateOperation({ current: targets.length })
      if (touchedIndex) await bridge.files.rescan().catch(() => undefined)
      await refresh()
      finishOperation('done')
      showToast('success', '已删除', label)
    } catch (err) {
      finishOperation('error', friendlyError(err))
      showToast('error', '删除失败', friendlyError(err))
    } finally {
      setBusy(false)
    }
  }

  function renameEntry(entry: FsEntry): void {
    if (busy) return
    setNameDialog({ mode: 'rename', value: entry.name, entry })
  }

  function createFolder(): void {
    if (busy) return
    setNameDialog({ mode: 'new', value: '新建文件夹' })
  }

  async function submitName(event: React.FormEvent): Promise<void> {
    event.preventDefault()
    if (!nameDialog || busy) return
    const next = nameDialog.value.trim()
    if (!next || next.includes('/') || next.includes('\\')) {
      showToast('error', '名称不合法', '名称不能为空，也不能包含 / 或 \\')
      return
    }
    setBusy(true)
    try {
      if (nameDialog.mode === 'new') {
        const target = joinPath(pathRef.current, next)
        await bridge.files.mkdir(target)
        await refresh()
        showToast('success', '文件夹已创建', next)
      } else if (nameDialog.entry) {
        const entry = nameDialog.entry
        if (next === entry.name) {
          setNameDialog(null)
          return
        }
        const target = joinPath(parentPath(entry.path), next)
        await bridge.files.rename(entry.path, target)
        if (isIndexPath(entry.path) || isIndexPath(target)) {
          await bridge.files.rescan().catch(() => undefined)
        }
        await refresh()
        showToast('success', '已重命名', next)
      }
      setNameDialog(null)
    } catch (err) {
      showToast('error', nameDialog.mode === 'new' ? '新建文件夹失败' : '重命名失败', friendlyError(err))
    } finally {
      setBusy(false)
    }
  }

  function handleFileInput(event: React.ChangeEvent<HTMLInputElement>): void {
    const files = Array.from(event.target.files ?? [])
    event.target.value = ''
    if (!files.length) return
    const sources = files.map((file) => {
      const relative = file.webkitRelativePath || ''
      const parts = relative.split('/')
      parts.pop()
      return { file, relDir: parts.join('/') }
    })
    void uploadSources(sources)
  }

  const breadcrumbs = useMemo(() => {
    const parts = path.split('/').filter(Boolean)
    return [
      { label: 'TF 卡', path: '/' },
      ...parts.map((part, index) => ({
        label: part,
        path: `/${parts.slice(0, index + 1).join('/')}`
      }))
    ]
  }, [path])

  const usedPct = listing && listing.capacity > 0 ? Math.min(100, (listing.used / listing.capacity) * 100) : 0
  const operationPct =
    operation && operation.total > 0 ? Math.min(100, Math.round((operation.current / operation.total) * 100)) : 0
  const operationLabel = operation
    ? operation.phase === 'done'
      ? operation.kind === 'move'
        ? '移动完成'
        : '删除完成'
      : operation.phase === 'error'
        ? operation.kind === 'move'
          ? '移动失败'
          : '删除失败'
        : operation.kind === 'move'
          ? '正在移动'
          : '正在删除'
    : ''
  const offline = !connected
  const notSupported = connected && !supported
  const cardMissing = supported && connected && listing !== null && !listing.ready

  if (offline || notSupported) {
    return (
      <div className="files-page">
        <div className="files-offline panel">
          <div className="files-offline-icon">{offline ? <WifiOff size={30} /> : <AlertCircle size={30} />}</div>
          <h2>{offline ? '连接 StoryBox 后管理文件' : '当前固件不支持文件管理'}</h2>
          <p>
            {offline
              ? '文件通过 USB 串口传输，需要先连接设备。连接后可以把 Windows 里的文件或文件夹直接拖进来。'
              : '请把包含 filexfer.cpp 的新固件烧录到设备，然后重新连接。'}
          </p>
          <button type="button" className="btn btn-primary" onClick={() => setView('device')}>
            前往设备与连接
          </button>
        </div>
      </div>
    )
  }

  return (
    <div
      className="files-page"
      onDragEnter={(event) => {
        if (!event.dataTransfer.types.includes('Files')) return
        event.preventDefault()
        dragDepth.current += 1
        setDragging(true)
      }}
      onDragOver={(event) => {
        if (!event.dataTransfer.types.includes('Files')) return
        event.preventDefault()
        event.dataTransfer.dropEffect = 'copy'
      }}
      onDragLeave={() => {
        dragDepth.current = Math.max(0, dragDepth.current - 1)
        if (dragDepth.current === 0) setDragging(false)
      }}
      onDrop={(event) => void handleDrop(event)}
    >
      <header className="page-header files-heading">
        <div className="page-header-copy">
          <div className="page-eyebrow">FILES</div>
          <h1 className="page-title">文件</h1>
          <p className="page-desc">像资源管理器一样管理 TF 卡。拖入文件或文件夹，或按 Ctrl+V 粘贴。</p>
        </div>
        <div className="files-heading-actions">
          <button type="button" className="btn btn-secondary" onClick={() => void refresh()} disabled={loading || busy}>
            <RefreshCw size={15} className={loading ? 'spin' : ''} /> 刷新
          </button>
          <button type="button" className="btn btn-secondary" onClick={() => void createFolder()} disabled={busy}>
            <FolderPlus size={15} /> 新建文件夹
          </button>
          <button
            type="button"
            className="btn btn-secondary"
            onClick={() => openMove()}
            disabled={busy || selectedEntries.length === 0}
            title="移动选中的文件或文件夹"
          >
            <Move size={15} /> 移动{selectedEntries.length > 1 ? ` (${selectedEntries.length})` : ''}
          </button>
          <button type="button" className="btn btn-secondary" onClick={() => void pasteFromClipboard()} disabled={busy}>
            <ClipboardPaste size={15} /> 粘贴
          </button>
          <button type="button" className="btn btn-secondary" onClick={() => folderInput.current?.click()} disabled={busy}>
            <UploadCloud size={15} /> 上传文件夹
          </button>
          <button type="button" className="btn btn-primary" onClick={() => fileInput.current?.click()} disabled={busy}>
            <Upload size={15} /> 上传文件
          </button>
        </div>
      </header>

      <input ref={fileInput} type="file" multiple hidden onChange={handleFileInput} />
      <input
        ref={folderInput}
        type="file"
        multiple
        hidden
        // @ts-expect-error webkitdirectory is not in React's input typings
        webkitdirectory=""
        directory=""
        onChange={handleFileInput}
      />

      <section className="explorer">
        <div className="explorer-toolbar">
          <div className="explorer-nav-buttons">
            <button type="button" className="explorer-icon-btn" onClick={goBack} disabled={historyIndex <= 0} title="后退">
              <ArrowLeft size={16} />
            </button>
            <button
              type="button"
              className="explorer-icon-btn"
              onClick={goForward}
              disabled={historyIndex >= history.length - 1}
              title="前进"
            >
              <ArrowRight size={16} />
            </button>
            <button type="button" className="explorer-icon-btn" onClick={goUp} disabled={path === '/'} title="上一级">
              <ArrowUp size={16} />
            </button>
            <button type="button" className="explorer-icon-btn" onClick={() => void refresh()} title="刷新">
              <RefreshCw size={15} className={loading ? 'spin' : ''} />
            </button>
          </div>

          <nav className="explorer-breadcrumb" aria-label="路径">
            {breadcrumbs.map((crumb, index) => (
              <span key={crumb.path} className="explorer-crumb-wrap">
                {index > 0 && <ChevronRight size={13} className="explorer-crumb-sep" />}
                <button
                  type="button"
                  className={`explorer-crumb ${crumb.path === path ? 'current' : ''}`}
                  onClick={() => void loadPath(crumb.path)}
                  onDragOver={(event) => {
                    if (event.dataTransfer.types.includes('application/x-storybox-entry')) event.preventDefault()
                  }}
                  onDrop={(event) => {
                    const paths = readDraggedPaths(event.dataTransfer)
                    if (!paths.length) return
                    event.preventDefault()
                    event.stopPropagation()
                    void movePaths(paths, crumb.path)
                  }}
                >
                  {crumb.label}
                </button>
              </span>
            ))}
          </nav>

          <label className="explorer-search">
            <Search size={14} />
            <input
              value={search}
              onChange={(event) => setSearch(event.target.value)}
              placeholder="搜索当前文件夹"
              aria-label="搜索当前文件夹"
            />
            {search && (
              <button type="button" className="explorer-clear" onClick={() => setSearch('')} aria-label="清除搜索">
                <X size={12} />
              </button>
            )}
          </label>

          <div className="explorer-view-toggle">
            <button
              type="button"
              className={view === 'details' ? 'active' : ''}
              onClick={() => setViewMode('details')}
              title="详细信息"
            >
              <List size={15} />
            </button>
            <button
              type="button"
              className={view === 'grid' ? 'active' : ''}
              onClick={() => setViewMode('grid')}
              title="大图标"
            >
              <LayoutGrid size={15} />
            </button>
          </div>
        </div>

        <div className="explorer-body">
          <aside className="explorer-side">
            <div className="explorer-side-title">快速访问</div>
            <div className="explorer-quick">
              {QUICK_LINKS.map((link) => {
                const Icon = link.icon
                return (
                  <button
                    key={link.path}
                    type="button"
                    className={`explorer-quick-item ${path === link.path ? 'active' : ''}`}
                    onClick={() => void loadPath(link.path)}
                    onDragOver={(event) => {
                      if (event.dataTransfer.types.includes('application/x-storybox-entry')) event.preventDefault()
                    }}
                    onDrop={(event) => {
                      const paths = readDraggedPaths(event.dataTransfer)
                      if (!paths.length) return
                      event.preventDefault()
                      event.stopPropagation()
                      void movePaths(paths, link.path)
                    }}
                  >
                    <Icon size={16} />
                    <span>{link.label}</span>
                  </button>
                )
              })}
            </div>

            <div className="explorer-side-title">此电脑</div>
            <div className="explorer-drive">
              <div className="explorer-drive-icon">
                <HardDrive size={20} />
              </div>
              <div className="explorer-drive-copy">
                <strong>TF 卡</strong>
                <span>{listing ? `${formatSize(listing.used)} / ${formatSize(listing.capacity)}` : '等待读取…'}</span>
              </div>
            </div>
            <div className="explorer-capacity">
              <div className="explorer-capacity-bar">
                <span style={{ width: `${usedPct}%` }} />
              </div>
              <div className="explorer-capacity-text">
                <span>已用 {listing ? formatSize(listing.used) : '—'}</span>
                <span>可用 {listing ? formatSize(listing.free) : '—'}</span>
              </div>
            </div>

            <div className="explorer-side-tip">
              <UploadCloud size={15} />
              <span>拖入文件或文件夹即可上传；Ctrl+V 可粘贴。</span>
            </div>
          </aside>

          <div className="explorer-main">
            {cardMissing ? (
              <div className="explorer-state">
                <HardDrive size={34} />
                <strong>未检测到 TF 卡</strong>
                <span>请插卡后点击刷新。</span>
              </div>
            ) : loading && entries.length === 0 ? (
              <div className="explorer-state">
                <Loader2 size={30} className="spin" />
                <strong>正在读取 TF 卡…</strong>
              </div>
            ) : error ? (
              <div className="explorer-state error">
                <AlertCircle size={32} />
                <strong>读取失败</strong>
                <span>{error}</span>
                <button type="button" className="btn btn-secondary" onClick={() => void refresh()}>
                  重试
                </button>
              </div>
            ) : sorted.length === 0 ? (
              <div className="explorer-state">
                <FolderOpen size={36} />
                <strong>{search ? '没有匹配的项目' : '这个文件夹是空的'}</strong>
                <span>{search ? '换个关键词试试。' : '把文件拖进来，或点击右上角上传。'}</span>
              </div>
            ) : view === 'details' ? (
              <div className="explorer-table-wrap">
                <table className="explorer-table">
                  <thead>
                    <tr>
                      <th className="col-select">
                        <button
                          type="button"
                          className={`row-check ${allSelected ? 'checked' : ''} ${partiallySelected ? 'mixed' : ''}`}
                          onClick={toggleSelectAll}
                          aria-label={allSelected ? '取消全选' : '全选'}
                          role="checkbox"
                          aria-checked={partiallySelected ? 'mixed' : allSelected}
                        >
                          {allSelected ? <Check size={12} /> : partiallySelected ? <Minus size={12} /> : null}
                        </button>
                      </th>
                      <th className="col-name">
                        <button type="button" onClick={() => toggleSort('name')}>
                          名称
                          {sort.key === 'name' && <ArrowDownUp size={12} />}
                        </button>
                      </th>
                      <th className="col-date">
                        <button type="button" onClick={() => toggleSort('mtime')}>
                          修改日期
                          {sort.key === 'mtime' && <ArrowDownUp size={12} />}
                        </button>
                      </th>
                      <th className="col-type">
                        <button type="button" onClick={() => toggleSort('type')}>
                          类型
                          {sort.key === 'type' && <ArrowDownUp size={12} />}
                        </button>
                      </th>
                      <th className="col-size">
                        <button type="button" onClick={() => toggleSort('size')}>
                          大小
                          {sort.key === 'size' && <ArrowDownUp size={12} />}
                        </button>
                      </th>
                    </tr>
                  </thead>
                  <tbody>
                    {sorted.map((entry) => {
                      const Icon = iconFor(entry)
                      const isSelected = selected.has(entry.path)
                      return (
                        <tr
                          key={entry.path}
                          className={`${isSelected ? 'selected' : ''} ${entry.dir ? 'is-dir' : ''}`}
                          draggable
                          onDragStart={(event) => {
                            const paths = isSelected
                              ? selectedEntries.map((item) => item.path)
                              : [entry.path]
                            if (!isSelected) setSelected(new Set([entry.path]))
                            event.dataTransfer.setData('application/x-storybox-entries', JSON.stringify(paths))
                            event.dataTransfer.setData('application/x-storybox-entry', entry.path)
                            event.dataTransfer.setData('text/plain', paths.join('\n'))
                            event.dataTransfer.effectAllowed = 'move'
                          }}
                          onDragOver={(event) => {
                            if (entry.dir) {
                              event.preventDefault()
                              event.dataTransfer.dropEffect = 'move'
                            }
                          }}
                          onDrop={(event) => {
                            const paths = readDraggedPaths(event.dataTransfer)
                            if (!paths.length || !entry.dir) return
                            event.preventDefault()
                            event.stopPropagation()
                            void movePaths(paths, entry.path)
                          }}
                          onClick={(event) => selectEntry(entry, event)}
                          onDoubleClick={() => openEntry(entry)}
                          onContextMenu={(event) => {
                            event.preventDefault()
                            if (!selected.has(entry.path)) setSelected(new Set([entry.path]))
                            setContext({ x: event.clientX, y: event.clientY, entry })
                          }}
                        >
                          <td className="col-select">
                            <button
                              type="button"
                              className={`row-check ${isSelected ? 'checked' : ''}`}
                              onClick={(event) => toggleEntry(entry, event)}
                              aria-label={`${isSelected ? '取消选择' : '选择'} ${entry.name}`}
                              role="checkbox"
                              aria-checked={isSelected}
                            >
                              {isSelected && <Check size={12} />}
                            </button>
                          </td>
                          <td className="col-name">
                            <span className={`file-icon ${entry.dir ? 'folder' : extension(entry.name)}`}>
                              <Icon size={18} />
                            </span>
                            <span className="file-name">{entry.name}</span>
                          </td>
                          <td className="col-date">{formatDate(entry.mtime)}</td>
                          <td className="col-type">{typeLabel(entry)}</td>
                          <td className="col-size">{entry.dir ? '—' : formatSize(entry.size)}</td>
                        </tr>
                      )
                    })}
                  </tbody>
                </table>
              </div>
            ) : (
              <div className="explorer-grid">
                {sorted.map((entry) => {
                  const Icon = iconFor(entry)
                  return (
                    <button
                      type="button"
                      key={entry.path}
                      className={`explorer-tile ${selected.has(entry.path) ? 'selected' : ''}`}
                      draggable
                      onDragStart={(event) => {
                        const paths = selected.has(entry.path)
                          ? selectedEntries.map((item) => item.path)
                          : [entry.path]
                        if (!selected.has(entry.path)) setSelected(new Set([entry.path]))
                        event.dataTransfer.setData('application/x-storybox-entries', JSON.stringify(paths))
                        event.dataTransfer.setData('application/x-storybox-entry', entry.path)
                        event.dataTransfer.setData('text/plain', paths.join('\n'))
                      }}
                      onDragOver={(event) => {
                        if (entry.dir) event.preventDefault()
                      }}
                      onDrop={(event) => {
                        const paths = readDraggedPaths(event.dataTransfer)
                        if (!paths.length || !entry.dir) return
                        event.preventDefault()
                        event.stopPropagation()
                        void movePaths(paths, entry.path)
                      }}
                      onClick={(event) => selectEntry(entry, event)}
                      onDoubleClick={() => openEntry(entry)}
                      onContextMenu={(event) => {
                        event.preventDefault()
                        if (!selected.has(entry.path)) setSelected(new Set([entry.path]))
                        setContext({ x: event.clientX, y: event.clientY, entry })
                      }}
                    >
                      <span className={`explorer-tile-icon ${entry.dir ? 'folder' : extension(entry.name)}`}>
                        <Icon size={38} />
                      </span>
                      <span className="explorer-tile-name">{entry.name}</span>
                      <span className="explorer-tile-meta">{entry.dir ? '文件夹' : formatSize(entry.size)}</span>
                    </button>
                  )
                })}
              </div>
            )}
          </div>
        </div>

        {operation && (
          <div className={`explorer-operation ${operation.phase}`}>
            <div className="explorer-operation-head">
              <span className="explorer-operation-label">
                {operationLabel}
                <em>
                  {operation.current}/{operation.total}
                </em>
              </span>
              <span className="explorer-operation-name">{operation.name}</span>
              <span className="explorer-operation-pct">
                {operation.indeterminate && operation.phase === 'running' ? '处理中…' : `${operationPct}%`}
              </span>
            </div>
            <div className={`explorer-operation-bar ${operation.indeterminate && operation.phase === 'running' ? 'indeterminate' : ''}`}>
              <span style={{ width: `${operation.indeterminate && operation.phase === 'running' ? 35 : operationPct}%` }} />
            </div>
            {operation.message && <div className="explorer-operation-message">{operation.message}</div>}
          </div>
        )}

        <footer className="explorer-status">
          <div>
            <span>{listing?.total ?? entries.length} 个项目</span>
            {selectedEntries.length > 0 && <span> · 已选择 {selectedEntries.length} 个</span>}
            {busy && (
              <span className="explorer-status-busy"> · {operation ? '正在处理' : '正在传输'}…</span>
            )}
          </div>
          <div>
            <span>{path}</span>
            <span> · 可用 {listing ? formatSize(listing.free) : '—'}</span>
          </div>
        </footer>

        {dragging && (
          <div className="files-drop-overlay">
            <div className="files-drop-card">
              <UploadCloud size={34} />
              <strong>松开即可上传到 {path}</strong>
              <span>支持文件、文件夹，粘贴同样可用</span>
            </div>
          </div>
        )}
      </section>

      {transfers.length > 0 && (
        <div className="files-transfers">
          <div className="files-transfers-head">
            <strong>传输</strong>
            <button
              type="button"
              className="files-transfers-clear"
              onClick={() => setTransfers((prev) => prev.filter((item) => item.phase !== 'done'))}
              title="清除已完成"
            >
              <Check size={13} />
            </button>
          </div>
          {transfers.slice(0, 5).map((item) => {
            const pct = item.total > 0 ? Math.min(100, (item.written / item.total) * 100) : 0
            return (
              <div key={item.id} className={`transfer-row ${item.phase}`}>
                <div className="transfer-row-top">
                  <span className="transfer-name">
                    {item.direction === 'upload' ? <Upload size={13} /> : <Download size={13} />}
                    {item.name}
                  </span>
                  <span className="transfer-pct">
                    {item.phase === 'done' ? '完成' : item.phase === 'error' ? '失败' : `${Math.round(pct)}%`}
                  </span>
                  <button
                    type="button"
                    className="transfer-close"
                    onClick={() => setTransfers((prev) => prev.filter((row) => row.id !== item.id))}
                    aria-label="移除传输记录"
                  >
                    <X size={12} />
                  </button>
                </div>
                <div className="transfer-bar">
                  <span style={{ width: `${item.phase === 'error' ? 100 : pct}%` }} />
                </div>
                {item.message && <div className="transfer-error">{item.message}</div>}
              </div>
            )
          })}
        </div>
      )}

      {context && (
        <div
          className="explorer-context"
          style={{ left: context.x, top: context.y }}
          onPointerDown={(event) => event.stopPropagation()}
        >
          {context.entry ? (
            <>
              {context.entry.dir && (
                <button
                  type="button"
                  onClick={() => {
                    setContext(null)
                    openEntry(context.entry as FsEntry)
                  }}
                >
                  <FolderOpen size={14} /> 打开
                </button>
              )}
              {!context.entry.dir && (
                <button
                  type="button"
                  onClick={() => {
                    setContext(null)
                    void downloadEntry(context.entry as FsEntry)
                  }}
                >
                  <Download size={14} /> 下载到本机…
                </button>
              )}
              <button
                type="button"
                onClick={() => {
                  setContext(null)
                  openMove(selected.has(context.entry!.path) ? selectedEntries : [context.entry as FsEntry])
                }}
              >
                <Move size={14} /> 移动到…{selectedEntries.length > 1 && selected.has(context.entry.path) ? ` (${selectedEntries.length})` : ''}
              </button>
              {selectedEntries.length <= 1 && (
                <button
                  type="button"
                  onClick={() => {
                    setContext(null)
                    void renameEntry(context.entry as FsEntry)
                  }}
                >
                  <Pencil size={14} /> 重命名
                </button>
              )}
              <div className="explorer-context-sep" />
              <button
                type="button"
                className="danger"
                onClick={() => {
                  setContext(null)
                  void deleteEntries(
                    selected.has(context.entry!.path) ? selectedEntries : [context.entry as FsEntry]
                  )
                }}
              >
                <Trash2 size={14} /> 删除
                {selectedEntries.length > 1 && selected.has(context.entry.path) ? ` (${selectedEntries.length})` : ''}
              </button>
            </>
          ) : (
            <>
              <button type="button" onClick={() => { setContext(null); void createFolder() }}>
                <FolderPlus size={14} /> 新建文件夹
              </button>
              <button type="button" onClick={() => { setContext(null); fileInput.current?.click() }}>
                <Upload size={14} /> 上传文件
              </button>
              <button type="button" onClick={() => { setContext(null); folderInput.current?.click() }}>
                <UploadCloud size={14} /> 上传文件夹
              </button>
              <div className="explorer-context-sep" />
              <button type="button" onClick={() => { setContext(null); void refresh() }}>
                <RefreshCw size={14} /> 刷新
              </button>
            </>
          )}
        </div>
      )}

      {moveOpen && (
        <div className="files-dialog-backdrop" onPointerDown={() => setMoveOpen(false)}>
          <div
            className="files-dialog files-move-dialog"
            onPointerDown={(event) => event.stopPropagation()}
          >
            <div className="files-move-head">
              <div>
                <h3>移动 {moveItems.length} 个项目</h3>
                <p>选择目标文件夹，文件会保留原名</p>
              </div>
              <button type="button" className="files-move-close" onClick={() => setMoveOpen(false)} aria-label="关闭">
                <X size={15} />
              </button>
            </div>
            <div className="files-move-location">
              <button
                type="button"
                className="explorer-icon-btn"
                onClick={() => void loadMoveDirs(parentPath(movePath) || '/')}
                disabled={movePath === '/' || moveLoading}
                title="上一级"
              >
                <ArrowUp size={15} />
              </button>
              <span className="files-move-path">{movePath}</span>
            </div>
            <div className="files-move-list">
              {moveLoading ? (
                <div className="files-move-empty"><Loader2 size={22} className="spin" /> 正在读取…</div>
              ) : moveDirs.length === 0 ? (
                <div className="files-move-empty"><FolderOpen size={24} /> 没有子文件夹</div>
              ) : (
                moveDirs.map((dir) => (
                  <button
                    key={dir.path}
                    type="button"
                    className="files-move-folder"
                    onClick={() => void loadMoveDirs(dir.path)}
                    onDoubleClick={() => void loadMoveDirs(dir.path)}
                  >
                    <span className="file-icon folder"><Folder size={17} /></span>
                    <span>{dir.name}</span>
                    <ChevronRight size={13} />
                  </button>
                ))
              )}
            </div>
            <div className="files-move-target">
              <span>目标：{movePath}</span>
              <div className="files-dialog-actions">
                <button type="button" className="btn btn-secondary" onClick={() => setMoveOpen(false)}>
                  取消
                </button>
                <button
                  type="button"
                  className="btn btn-primary"
                  onClick={() => void moveEntries(moveItems, movePath)}
                  disabled={busy || moveLoading}
                >
                  <Move size={14} /> 移动到此处
                </button>
              </div>
            </div>
          </div>
        </div>
      )}

      {deleteTargets && (
        <div className="files-dialog-backdrop" onPointerDown={() => setDeleteTargets(null)}>
          <div className="files-dialog" onPointerDown={(event) => event.stopPropagation()}>
            <h3>删除确认</h3>
            <p>
              {deleteTargets.length === 1
                ? `确定要删除“${deleteTargets[0].name}”吗？`
                : `确定要删除选中的 ${deleteTargets.length} 个项目吗？`}
              此操作不可撤销。
            </p>
            <div className="files-dialog-actions">
              <button type="button" className="btn btn-secondary" onClick={() => setDeleteTargets(null)}>
                取消
              </button>
              <button
                type="button"
                className="btn btn-danger"
                onClick={() => void confirmDelete()}
                disabled={busy}
              >
                <Trash2 size={14} /> 删除
              </button>
            </div>
          </div>
        </div>
      )}

      {nameDialog && (
        <div className="files-dialog-backdrop" onPointerDown={() => setNameDialog(null)}>
          <form
            className="files-dialog"
            onSubmit={(event) => void submitName(event)}
            onPointerDown={(event) => event.stopPropagation()}
          >
            <h3>{nameDialog.mode === 'new' ? '新建文件夹' : '重命名'}</h3>
            <p>{nameDialog.mode === 'new' ? `将在 ${path} 下创建` : nameDialog.entry?.name}</p>
            <input
              autoFocus
              value={nameDialog.value}
              onChange={(event) => setNameDialog({ ...nameDialog, value: event.target.value })}
              onKeyDown={(event) => {
                if (event.key === 'Escape') setNameDialog(null)
              }}
              aria-label={nameDialog.mode === 'new' ? '文件夹名称' : '新名称'}
            />
            <div className="files-dialog-actions">
              <button type="button" className="btn btn-secondary" onClick={() => setNameDialog(null)}>
                取消
              </button>
              <button type="submit" className="btn btn-primary" disabled={busy || !nameDialog.value.trim()}>
                {nameDialog.mode === 'new' ? '创建' : '保存'}
              </button>
            </div>
          </form>
        </div>
      )}

      <div className="files-mobile-actions">
        <button type="button" className="btn btn-secondary" onClick={() => fileInput.current?.click()}>
          <Upload size={15} />
        </button>
        <button type="button" className="btn btn-secondary" onClick={() => folderInput.current?.click()}>
          <UploadCloud size={15} />
        </button>
        <button type="button" className="btn btn-secondary" onClick={() => void createFolder()}>
          <FolderPlus size={15} />
        </button>
      </div>
    </div>
  )
}
