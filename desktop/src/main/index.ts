import { app, BrowserWindow, clipboard, dialog, ipcMain, Menu, screen, shell } from 'electron'
import { existsSync, mkdirSync } from 'node:fs'
import { open, readdir, stat, unlink, rename } from 'node:fs/promises'
import { basename, join } from 'node:path'
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { fileURLToPath } from 'node:url'
import { SettingsStore } from './settings'
import { NfcSession, listPorts } from './serial'
import { transferWindow } from '@shared/transfer'
import type { AppSettings, FsProgress, RawReadRequest } from '@shared/types'

// 独立数据目录：避免复用 Electron 默认 profile 时被旧缓存/单例锁影响。
// 软件渲染在这类没有 GPU 进程权限的环境里更稳定，桌面工具的性能足够。
const userDataDir =
  process.env.NFC_USER_DATA_DIR || join(app.getPath('appData'), 'StoryBox-NFC-Studio')
try {
  mkdirSync(userDataDir, { recursive: true })
} catch {
  /* 目录创建失败时 Electron 仍会使用内存 profile */
}
app.setPath('userData', userDataDir)
app.disableHardwareAcceleration()

const DEMO = process.argv.includes('--demo') || process.env.NFC_DEMO === '1'
const settings = new SettingsStore()
const session = new NfcSession(settings)

let mainWindow: BrowserWindow | null = null

function send(channel: string, payload: unknown): void {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send(channel, payload)
  }
}

function remoteJoin(dir: string, name: string): string {
  return dir === '/' ? `/${name}` : `${dir}/${name}`
}

async function ensureRemoteDirectory(remoteDir: string, cache: Set<string>): Promise<void> {
  if (remoteDir === '/' || !remoteDir) return
  const parts = remoteDir.split('/').filter(Boolean)
  let current = ''
  for (const part of parts) {
    current = `${current}/${part}`
    if (cache.has(current)) continue
    try {
      await session.fsMkdir(current)
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err)
      if (!message.includes('EXISTS')) throw err
    }
    cache.add(current)
  }
}

async function uploadLocalEntry(
  localPath: string,
  remoteDir: string,
  cache: Set<string>,
  stats: { files: number; failed: number; touchedIndex: boolean }
): Promise<void> {
  const info = await stat(localPath)
  const name = basename(localPath)
  const remotePath = remoteJoin(remoteDir, name)
  if (info.isDirectory()) {
    await ensureRemoteDirectory(remotePath, cache)
    const children = await readdir(localPath, { withFileTypes: true })
    for (const child of children) {
      try {
        await uploadLocalEntry(join(localPath, child.name), remotePath, cache, stats)
      } catch {
        stats.failed += 1
      }
    }
    return
  }
  if (!info.isFile()) return

  const id = `up-${Date.now()}-${Math.random().toString(16).slice(2)}`
  const total = info.size
  let written = 0
  let xid = 0
  let lastProgress = 0
  let handle: Awaited<ReturnType<typeof open>> | undefined
  send('fs:progress', {
    id,
    direction: 'upload',
    name,
    path: remotePath,
    written: 0,
    total,
    phase: 'start'
  } satisfies FsProgress)

  try {
    const start = await session.fsWriteStart(remotePath, total, true)
    xid = start.xid
    handle = await open(localPath, 'r')
    const buffer = Buffer.alloc(64 * 1024)
    let offset = 0
    while (offset < total) {
      const { bytesRead } = await handle.read(buffer, 0, buffer.length, offset)
      if (bytesRead <= 0) break
      const data = new Uint8Array(buffer.subarray(0, bytesRead))
      await session.fsWriteChunk(xid, offset, data)
      offset += bytesRead
      written = offset
      if (written - lastProgress >= 256 * 1024 || written === total) {
        lastProgress = written
        send('fs:progress', {
          id,
          direction: 'upload',
          name,
          path: remotePath,
          written,
          total,
          phase: 'progress'
        } satisfies FsProgress)
      }
    }
    const result = await session.fsWriteEnd(xid)
    xid = 0
    if (!result.complete) throw new Error('SHORT_WRITE: 文件写入长度不完整')
    stats.files += 1
    if (remotePath.startsWith('/music') || remotePath.startsWith('/story')) stats.touchedIndex = true
    send('fs:progress', {
      id,
      direction: 'upload',
      name,
      path: remotePath,
      written,
      total,
      phase: 'done'
    } satisfies FsProgress)
  } catch (err) {
    if (xid) await session.fsCancel(xid).catch(() => undefined)
    send('fs:progress', {
      id,
      direction: 'upload',
      name,
      path: remotePath,
      written,
      total,
      phase: 'error',
      message: err instanceof Error ? err.message : String(err)
    } satisfies FsProgress)
    throw err
  } finally {
    if (handle) await handle.close().catch(() => undefined)
  }
}

async function readClipboardPaths(): Promise<string[]> {
  const paths = new Set<string>()
  const addText = (text: string): void => {
    for (const raw of text.split(/\r?\n/)) {
      let line = raw.trim().replace(/^"(.*)"$/, '$1')
      if (!line) continue
      if (line.startsWith('file://')) {
        try {
          line = fileURLToPath(line)
        } catch {
          continue
        }
      }
      if (!/^[A-Za-z]:[\\/]/.test(line) && !line.startsWith('\\\\')) continue
      if (existsSync(line)) paths.add(line)
    }
  }

  try {
    addText(await clipboard.readText())
  } catch {
    /* clipboard can be empty or unavailable */
  }
  try {
    const items = await clipboard.read()
    for (const item of items) {
      for (const type of item.types) {
        if (!/uri-list|plain|text/i.test(type)) continue
        try {
          const value = await item.getType(type)
          if (value instanceof Blob) addText(await value.text())
        } catch {
          /* skip unsupported clipboard type */
        }
      }
    }
  } catch {
    /* clipboard read is best-effort */
  }

  // Windows Explorer 复制文件夹时主要提供 CF_HDROP，Chromium 的 paste
  // 事件通常拿不到目录。这里用系统剪贴板把路径补全，支持文件和文件夹。
  if (process.platform === 'win32' && paths.size === 0) {
    try {
      const { stdout } = await promisify(execFile)(
        'powershell.exe',
        [
          '-NoProfile',
          '-NonInteractive',
          '-Command',
          'Get-Clipboard -Format FileDropList | ForEach-Object { $_.FullName }'
        ],
        { timeout: 4000, maxBuffer: 1024 * 1024, windowsHide: true }
      )
      addText(stdout)
    } catch {
      /* no file drop list */
    }
  }

  return Array.from(paths)
}

function createWindow(): void {
  const workArea = screen.getPrimaryDisplay().workAreaSize
  const width = Math.max(640, Math.min(1480, workArea.width - 32))
  const height = Math.max(560, Math.min(940, workArea.height - 32))
  mainWindow = new BrowserWindow({
    width,
    height,
    minWidth: Math.min(1120, width),
    minHeight: Math.min(720, height),
    show: false,
    frame: false,
    thickFrame: true,
    roundedCorners: true,
    backgroundColor: '#f4f6fb',
    autoHideMenuBar: true,
    title: 'StoryBox NFC Studio',
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  })
  mainWindow.center()
  mainWindow.setMenuBarVisibility(false)
  mainWindow.removeMenu()
  mainWindow.on('maximize', () => send('win:maximized', true))
  mainWindow.on('unmaximize', () => send('win:maximized', false))

  let shown = false
  const showWindow = (): void => {
    if (shown || !mainWindow || mainWindow.isDestroyed()) return
    shown = true
    mainWindow.show()
    mainWindow.focus()
  }

  // ready-to-show 在个别软件渲染/受管环境里可能不触发，额外用 did-finish-load
  // 和定时器兜底，避免进程存在但用户看不到界面。
  mainWindow.once('ready-to-show', showWindow)
  mainWindow.webContents.once('did-finish-load', () => {
    setTimeout(showWindow, 150)
  })
  setTimeout(showWindow, 2500)

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:/i.test(url)) void shell.openExternal(url)
    return { action: 'deny' }
  })

  if (process.env.ELECTRON_RENDERER_URL) {
    void mainWindow.loadURL(process.env.ELECTRON_RENDERER_URL)
  } else {
    void mainWindow.loadFile(join(__dirname, '../renderer/index.html'))
  }

  mainWindow.on('closed', () => {
    mainWindow = null
  })
}

function wireSession(): void {
  session.on('status', (status) => send('conn:status', status))
  session.on('device', (event) => send('device:event', event))
  session.on('log', (entry) => send('log:entry', entry))
}

function registerIpc(): void {
  ipcMain.on('app:version-sync', (event) => {
    event.returnValue = app.getVersion()
  })

  ipcMain.handle('app:info', () => ({
    version: app.getVersion(),
    platform: process.platform,
    demo: DEMO
  }))

  ipcMain.handle('ports:list', () => listPorts())
  ipcMain.handle('conn:get', () => session.getStatus())
  ipcMain.handle('conn:connect', (_event, path?: string) => session.connect(path))
  ipcMain.handle('conn:auto', () => session.autoConnect())
  ipcMain.handle('conn:disconnect', async () => {
    await session.disconnect()
    return session.getStatus()
  })

  ipcMain.handle('device:hello', () => session.hello())
  ipcMain.handle('device:nfc-status', () => session.nfcStatus())
  ipcMain.handle('device:poll', (_event, on: boolean) => session.setPolling(Boolean(on)))
  ipcMain.handle('device:refresh', () => session.refresh())
  ipcMain.handle('device:read-raw', (_event, req: RawReadRequest) => session.readRaw(req))
  ipcMain.handle('device:write-ndef', (_event, req: { ndef: string; verify?: boolean }) =>
    session.writeNdef(req.ndef, req.verify !== false)
  )
  ipcMain.handle(
    'device:write-raw',
    (_event, req: { kind: 'page' | 'block'; start: number; data: string; key?: string }) =>
      session.writeRaw(req)
  )
  ipcMain.handle('device:ping', () => session.ping())

  ipcMain.on('win:minimize', () => mainWindow?.minimize())
  ipcMain.on('win:close', () => mainWindow?.close())
  ipcMain.handle('win:toggle-maximize', () => {
    if (!mainWindow) return false
    if (mainWindow.isMaximized()) mainWindow.unmaximize()
    else mainWindow.maximize()
    return mainWindow.isMaximized()
  })
  ipcMain.handle('win:is-maximized', () => mainWindow?.isMaximized() ?? false)

  ipcMain.handle('fs:list', (_event, req: { path: string; offset?: number; limit?: number }) =>
    session.fsList(req.path, req.offset ?? 0, req.limit ?? 64)
  )
  ipcMain.handle('fs:mkdir', (_event, path: string) => session.fsMkdir(path))
  ipcMain.handle('fs:remove', (_event, path: string) => session.fsRemove(path))
  ipcMain.handle('fs:rename', (_event, req: { from: string; to: string }) =>
    session.fsRename(req.from, req.to)
  )
  ipcMain.handle('fs:read-start', (_event, path: string) => session.fsReadStart(path))
  ipcMain.handle('fs:session-begin', () => session.fsSessionBegin())
  ipcMain.handle('fs:session-end', () => session.fsSessionEnd())
  ipcMain.handle('fs:read-chunk', (_event, req: { xid: number; offset: number }) =>
    session.fsReadChunk(req.xid, req.offset)
  )
  ipcMain.handle('fs:read-end', (_event, xid: number) => session.fsReadEnd(xid))
  ipcMain.handle(
    'fs:write-start',
    (_event, req: { path: string; size: number; overwrite?: boolean }) =>
      session.fsWriteStart(req.path, req.size, req.overwrite !== false)
  )
  ipcMain.handle(
    'fs:write-chunk',
    (_event, req: { xid: number; offset: number; data: Uint8Array | ArrayBuffer }) =>
      session.fsWriteChunk(
        req.xid,
        req.offset,
        req.data instanceof Uint8Array ? req.data : new Uint8Array(req.data)
      )
  )
  ipcMain.handle('fs:write-end', (_event, xid: number) => session.fsWriteEnd(xid))
  ipcMain.handle('fs:cancel', (_event, xid: number) => session.fsCancel(xid))
  ipcMain.handle('fs:rescan', () => session.fsRescan())
  ipcMain.handle('fs:clipboard-paths', () => readClipboardPaths())
  ipcMain.handle(
    'fs:upload-paths',
    async (_event, req: { paths: string[]; remoteDir: string }) => {
      const cache = new Set<string>()
      const stats = { files: 0, failed: 0, touchedIndex: false }
      await session.fsSessionBegin()
      try {
        for (const localPath of req.paths) {
          try {
            await uploadLocalEntry(localPath, req.remoteDir || '/', cache, stats)
          } catch {
            stats.failed += 1
          }
        }
        if (stats.touchedIndex) {
          await session.fsRescan().catch(() => undefined)
        }
        return { files: stats.files, failed: stats.failed }
      } finally { await session.fsSessionEnd() }
    }
  )

  ipcMain.handle(
    'fs:download',
    async (_event, req: { path: string; defaultName: string }): Promise<{ canceled: boolean; path?: string; size: number }> => {
      const defaultName = basename(req.defaultName || req.path.split('/').pop() || 'storybox-file')
      const options = {
        title: '从 StoryBox 下载文件',
        defaultPath: join(app.getPath('downloads'), defaultName)
      }
      const result = mainWindow
        ? await dialog.showSaveDialog(mainWindow, options)
        : await dialog.showSaveDialog(options)
      if (result.canceled || !result.filePath) return { canceled: true, size: 0 }

      const progressId = `dl-${Date.now()}`
      const totalFallback = 0
      let handle: Awaited<ReturnType<typeof open>> | undefined
      let started = false
      let xid = 0
      let written = 0
      let total = totalFallback
      const temporaryPath = `${result.filePath}.storybox-${Date.now()}.part`
      let temporaryCreated = false
      await session.fsSessionBegin()
      try {
        const start = await session.fsReadStart(req.path)
        started = true
        xid = start.xid
        total = start.size
        handle = await open(temporaryPath, 'wx')
        temporaryCreated = true
        send('fs:progress', {
          id: progressId,
          direction: 'download',
          name: start.name,
          path: req.path,
          written: 0,
          total,
          phase: 'start'
        } satisfies FsProgress)

        let offset = 0
        let lastProgress = 0
        while (offset < total) {
          const jobs: (() => ReturnType<typeof session.fsReadChunk>)[] = []
          for (let i = 0; i < session.transferWindowSize() && offset + i * start.chunk < total; i++) {
            const position = offset + i * start.chunk
            jobs.push(() => session.fsReadChunk(start.xid, position))
          }
          const chunks = await transferWindow(jobs)
          for (const chunk of chunks) {
            if (chunk.offset !== offset || chunk.len !== Math.min(start.chunk, total - offset)) {
              throw new Error('VERIFY: 下载数据不连续或长度错误')
            }
            let localOffset = 0
            while (localOffset < chunk.len) {
              const result = await handle.write(chunk.data, localOffset, chunk.len - localOffset)
              if (!result.bytesWritten) throw new Error('IO: 本机文件写入失败')
              localOffset += result.bytesWritten
            }
            offset += chunk.len
            written = offset
            if (written - lastProgress >= 256 * 1024 || chunk.eof) {
              lastProgress = written
              send('fs:progress', {
                id: progressId,
                direction: 'download',
                name: start.name,
                path: req.path,
                written,
                total,
                phase: 'progress'
              } satisfies FsProgress)
            }
            if (chunk.eof) break
          }
        }
        await session.fsReadEnd(start.xid)
        started = false
        await handle.close()
        handle = undefined
        await rename(temporaryPath, result.filePath)
        temporaryCreated = false
        send('fs:progress', {
          id: progressId,
          direction: 'download',
          name: start.name,
          path: req.path,
          written,
          total,
          phase: 'done'
        } satisfies FsProgress)
        return { canceled: false, path: result.filePath, size: written }
      } catch (err) {
        if (started) await session.fsReadEnd(xid).catch(() => undefined)
        if (handle) await handle.close().catch(() => undefined)
        if (temporaryCreated) await unlink(temporaryPath).catch(() => undefined)
        send('fs:progress', {
          id: progressId,
          direction: 'download',
          name: defaultName,
          path: req.path,
          written,
          total,
          phase: 'error',
          message: err instanceof Error ? err.message : String(err)
        } satisfies FsProgress)
        throw err
      } finally {
        await session.fsSessionEnd()
      }
    }
  )

  ipcMain.handle('settings:get', () => settings.get())
  ipcMain.handle('settings:set', (_event, patch: Partial<AppSettings>) => settings.set(patch))
}

app.whenReady().then(async () => {
  app.setAppUserModelId('com.storybox.nfcstudio')
  Menu.setApplicationMenu(null)
  await settings.load()
  wireSession()
  registerIpc()
  createWindow()

  if (!DEMO) {
    const prefs = settings.get()
    if (prefs.autoConnect) {
      setTimeout(() => {
        void session.autoConnect()
      }, 600)
    }
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  void session.disconnect()
  if (process.platform !== 'darwin') app.quit()
})

app.on('before-quit', () => {
  void session.disconnect()
})
