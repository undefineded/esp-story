/* 渲染进程与主进程共享的数据结构 (不含 Node / Electron 依赖) */

export type ConnectionState = 'disconnected' | 'scanning' | 'connecting' | 'connected' | 'error';

export type TagKind =
  | 'ntag210'
  | 'ntag212'
  | 'ntag213'
  | 'ntag215'
  | 'ntag216'
  | 'ultralight'
  | 'mifare-classic-1k'
  | 'mifare-classic-4k'
  | 'mifare-mini'
  | 'iso14443a-4'
  | 'unknown';

export interface RawCard {
  uid: string;
  uidLen: number;
  atqa: string;
  sak: string;
  kind: TagKind;
  type: string;
  std: string;
  type2: boolean;
  writable: boolean;
  formatted: boolean;
  cap: number;
  userStart: number;
  userPages: number;
  userTotal: number;
  userBytes: number;
  version?: string;
  cc?: string;
  lock?: string;
  data: string;
  block0?: string;
}

export interface NfcStatus {
  present: boolean;
  chip: string;
  fw: string;
  i2c: string;
  polling: boolean;
  busy: boolean;
  uptime: number;
  heap: number;
  card?: RawCard;
}

export interface DeviceInfo {
  fw: string;
  proto: number;
  board: string;
  name: string;
  role: string;
  features: string[];
  nfc: NfcStatus;
}

export type NdefKind = 'text' | 'uri' | 'mime' | 'external' | 'empty' | 'unknown';

export interface NdefRecord {
  id: string;
  tnf: number;
  type: string;
  kind: NdefKind;
  typeLabel: string;
  value: string;
  language?: string;
  uri?: string;
  payloadHex: string;
}

export interface NdefParse {
  ok: boolean;
  formatted: boolean;
  records: NdefRecord[];
  messageHex: string;
  tlvLength: number;
  error?: string;
}

export interface NfcCard extends RawCard {
  firstSeen: number;
  lastSeen: number;
  ndef: NdefParse;
}

export interface PortInfo {
  path: string;
  manufacturer?: string;
  friendlyName?: string;
  vendorId?: string;
  productId?: string;
  serialNumber?: string;
}

export interface ConnectionStatus {
  state: ConnectionState;
  port?: string;
  message?: string;
  device?: DeviceInfo;
}

export interface WriteStatus {
  phase: 'idle' | 'start' | 'writing' | 'verifying' | 'done' | 'error';
  pct: number;
  written: number;
  ok: boolean;
  code?: string;
  message?: string;
}

export type DeviceEvent =
  | { type: 'module'; present: boolean; fw?: string; chip?: string; i2c?: string; polling?: boolean }
  | { type: 'card'; card: RawCard }
  | { type: 'removed' }
  | { type: 'write'; phase: 'start' | 'writing' | 'verifying' | 'done' | 'error'; pct?: number; written?: number; ok?: boolean; code?: string; msg?: string };

export interface LogEntry {
  id: number;
  ts: number;
  dir: 'in' | 'out' | 'sys' | 'error';
  text: string;
}

export interface ActivityEntry {
  id: string;
  ts: number;
  kind: 'card' | 'write' | 'connect' | 'error' | 'system';
  title: string;
  detail?: string;
  cardUid?: string;
}

export interface WriteRequest {
  kind: 'text' | 'uri' | 'raw';
  text?: string;
  language?: string;
  uri?: string;
  verify: boolean;
}

export interface AppSettings {
  autoConnect: boolean;
  lastPort?: string;
  baudRate: number;
}

export interface RawReadRequest {
  kind: 'page' | 'block';
  start: number;
  count: number;
  key?: string;
}

export interface RawReadResult {
  kind: 'page' | 'block';
  start: number;
  len: number;
  hex: string;
}

export interface WriteAccepted {
  accepted: boolean;
  len: number;
}

/* ---------- TF card file manager ---------- */

export interface FsEntry {
  name: string;
  path: string;
  dir: boolean;
  size: number;
  mtime: number;
}

export interface FsListing {
  path: string;
  parent: string;
  entries: FsEntry[];
  total: number;
  offset: number;
  limit: number;
  ready: boolean;
  capacity: number;
  used: number;
  free: number;
}

export interface FsReadStart {
  xid: number;
  path: string;
  name: string;
  size: number;
  chunk: number;
}

export interface FsReadChunk {
  data: Uint8Array;
  offset: number;
  len: number;
  size: number;
  eof: boolean;
}

export interface FsWriteStart {
  xid: number;
  path: string;
  size: number;
  chunk: number;
}

export type FsProgressPhase = 'start' | 'progress' | 'done' | 'error';

export interface FsProgress {
  id: string;
  direction: 'upload' | 'download';
  name: string;
  path: string;
  written: number;
  total: number;
  phase: FsProgressPhase;
  message?: string;
}

export interface FsDownloadResult {
  canceled: boolean;
  path?: string;
  size: number;
}

export interface WindowControls {
  minimize(): void;
  toggleMaximize(): Promise<boolean>;
  close(): void;
  isMaximized(): Promise<boolean>;
  onMaximized(cb: (maximized: boolean) => void): () => void;
}

export interface FileBridge {
  list(path: string, offset?: number, limit?: number): Promise<FsListing>;
  mkdir(path: string): Promise<void>;
  remove(path: string): Promise<void>;
  rename(from: string, to: string): Promise<void>;
  readStart(path: string): Promise<FsReadStart>;
  readChunk(xid: number, offset: number): Promise<FsReadChunk>;
  readEnd(xid: number): Promise<void>;
  writeStart(path: string, size: number, overwrite?: boolean): Promise<FsWriteStart>;
  writeChunk(xid: number, offset: number, data: Uint8Array): Promise<{ written: number }>;
  writeEnd(xid: number): Promise<{ size: number; complete: boolean }>;
  cancel(xid: number): Promise<void>;
  rescan(): Promise<{ tracks: number; categories: number }>;
  download(path: string, defaultName: string): Promise<FsDownloadResult>;
  clipboardPaths(): Promise<string[]>;
  uploadPaths(paths: string[], remoteDir: string): Promise<{ files: number; failed: number }>;
}

export interface NfcBridge {
  isDemo: boolean;
  platform: string;
  appVersion: string;
  listPorts(): Promise<PortInfo[]>;
  getConnection(): Promise<ConnectionStatus>;
  connect(path?: string): Promise<ConnectionStatus>;
  disconnect(): Promise<void>;
  autoConnect(): Promise<ConnectionStatus>;
  onConnection(cb: (status: ConnectionStatus) => void): () => void;
  onDeviceEvent(cb: (event: DeviceEvent) => void): () => void;
  onLog(cb: (entry: LogEntry) => void): () => void;
  hello(): Promise<DeviceInfo>;
  nfcStatus(): Promise<NfcStatus>;
  setPolling(on: boolean): Promise<NfcStatus>;
  refresh(): Promise<void>;
  readRaw(req: RawReadRequest): Promise<RawReadResult>;
  writeNdef(req: { ndef: string; verify?: boolean }): Promise<WriteAccepted>;
  writeRaw(req: { kind: 'page' | 'block'; start: number; data: string; key?: string }): Promise<WriteAccepted>;
  ping(): Promise<{ uptime: number; heap: number }>;
  files: FileBridge;
  onFsProgress(cb: (progress: FsProgress) => void): () => void;
  windowControls: WindowControls;
  getSettings(): Promise<AppSettings>;
  setSettings(patch: Partial<AppSettings>): Promise<AppSettings>;
  demo?: {
    placeCard(overrides?: Partial<RawCard>): void;
    removeCard(): void;
    writeNdef(hex: string): Promise<void>;
  };
}
