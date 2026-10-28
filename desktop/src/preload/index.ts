import { contextBridge, ipcRenderer } from 'electron'
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
  RawReadRequest,
  RawReadResult,
  WriteAccepted
} from '@shared/types'

function on<T>(channel: string, callback: (payload: T) => void): () => void {
  const listener = (_event: Electron.IpcRendererEvent, payload: T): void => callback(payload)
  ipcRenderer.on(channel, listener)
  return () => ipcRenderer.removeListener(channel, listener)
}

const demo = process.argv.includes('--demo') || process.env.NFC_DEMO === '1'
const appVersion =
  (process.env.npm_package_version as string | undefined) ??
  (ipcRenderer.sendSync('app:version-sync') as string) ??
  '1.0.0'

const files: FileBridge = {
  sessionBegin: () => ipcRenderer.invoke('fs:session-begin') as Promise<void>,
  sessionEnd: () => ipcRenderer.invoke('fs:session-end') as Promise<void>,
  list: (path, offset = 0, limit = 64) =>
    ipcRenderer.invoke('fs:list', { path, offset, limit }) as Promise<FsListing>,
  mkdir: (path) => ipcRenderer.invoke('fs:mkdir', path) as Promise<void>,
  remove: (path) => ipcRenderer.invoke('fs:remove', path) as Promise<void>,
  rename: (from, to) => ipcRenderer.invoke('fs:rename', { from, to }) as Promise<void>,
  readStart: (path) => ipcRenderer.invoke('fs:read-start', path) as Promise<FsReadStart>,
  readChunk: (xid, offset) =>
    ipcRenderer.invoke('fs:read-chunk', { xid, offset }) as Promise<FsReadChunk>,
  readEnd: (xid) => ipcRenderer.invoke('fs:read-end', xid) as Promise<void>,
  writeStart: (path, size, overwrite = true) =>
    ipcRenderer.invoke('fs:write-start', { path, size, overwrite }) as Promise<FsWriteStart>,
  writeChunk: (xid, offset, data) =>
    ipcRenderer.invoke('fs:write-chunk', { xid, offset, data }) as Promise<{ written: number }>,
  writeEnd: (xid) =>
    ipcRenderer.invoke('fs:write-end', xid) as Promise<{ size: number; complete: boolean }>,
  cancel: (xid) => ipcRenderer.invoke('fs:cancel', xid) as Promise<void>,
  rescan: () =>
    ipcRenderer.invoke('fs:rescan') as Promise<{ tracks: number; categories: number }>,
  download: (path, defaultName) =>
    ipcRenderer.invoke('fs:download', { path, defaultName }) as Promise<FsDownloadResult>,
  clipboardPaths: () => ipcRenderer.invoke('fs:clipboard-paths') as Promise<string[]>,
  uploadPaths: (paths, remoteDir) =>
    ipcRenderer.invoke('fs:upload-paths', { paths, remoteDir }) as Promise<{
      files: number
      failed: number
    }>
}

const bridge: NfcBridge = {
  isDemo: demo,
  platform: process.platform,
  appVersion,
  listPorts: () => ipcRenderer.invoke('ports:list') as Promise<PortInfo[]>,
  getConnection: () => ipcRenderer.invoke('conn:get') as Promise<ConnectionStatus>,
  connect: (path?: string) => ipcRenderer.invoke('conn:connect', path) as Promise<ConnectionStatus>,
  disconnect: () => ipcRenderer.invoke('conn:disconnect') as Promise<void>,
  autoConnect: () => ipcRenderer.invoke('conn:auto') as Promise<ConnectionStatus>,
  onConnection: (cb) => on<ConnectionStatus>('conn:status', cb),
  onDeviceEvent: (cb) => on<DeviceEvent>('device:event', cb),
  onLog: (cb) => on<LogEntry>('log:entry', cb),
  hello: () => ipcRenderer.invoke('device:hello') as Promise<DeviceInfo>,
  nfcStatus: () => ipcRenderer.invoke('device:nfc-status') as Promise<NfcStatus>,
  setPolling: (onValue: boolean) => ipcRenderer.invoke('device:poll', onValue) as Promise<NfcStatus>,
  refresh: () => ipcRenderer.invoke('device:refresh') as Promise<void>,
  readRaw: (req: RawReadRequest) => ipcRenderer.invoke('device:read-raw', req) as Promise<RawReadResult>,
  writeNdef: (req: { ndef: string; verify?: boolean }) =>
    ipcRenderer.invoke('device:write-ndef', req) as Promise<WriteAccepted>,
  writeRaw: (req: { kind: 'page' | 'block'; start: number; data: string; key?: string }) =>
    ipcRenderer.invoke('device:write-raw', req) as Promise<WriteAccepted>,
  ping: () => ipcRenderer.invoke('device:ping') as Promise<{ uptime: number; heap: number }>,
  files,
  onFsProgress: (cb) => on<FsProgress>('fs:progress', cb),
  windowControls: {
    minimize: () => ipcRenderer.send('win:minimize'),
    toggleMaximize: () => ipcRenderer.invoke('win:toggle-maximize') as Promise<boolean>,
    close: () => ipcRenderer.send('win:close'),
    isMaximized: () => ipcRenderer.invoke('win:is-maximized') as Promise<boolean>,
    onMaximized: (cb) => on<boolean>('win:maximized', cb)
  },
  getSettings: () => ipcRenderer.invoke('settings:get') as Promise<AppSettings>,
  setSettings: (patch: Partial<AppSettings>) =>
    ipcRenderer.invoke('settings:set', patch) as Promise<AppSettings>
}

contextBridge.exposeInMainWorld('nfcBridge', bridge)
