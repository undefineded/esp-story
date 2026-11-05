/**
 * Host half of the serial-console bundle.
 *
 * Owns the real work the right-sidebar panel shows:
 *   - a PowerShell child process that keeps one COM port open and streams its bytes,
 *   - cached COM-port enumeration with device names and plug status,
 *   - a PlatformIO `run -e <env> -t upload` child whose output becomes flash progress.
 *
 * The browser half cannot reach Node, so everything is published as JSON on one
 * loopback-only prefix route that the Client module polls:
 *
 *   GET  {ROUTE}/state?since=<seq>[&ports=1]   serial chunks + serial/flash/ports state
 *   POST {ROUTE}/connect                       { port, baud }
 *   POST {ROUTE}/disconnect
 *   POST {ROUTE}/clear
 *   POST {ROUTE}/flash                         { env?, port? }
 *   POST {ROUTE}/flash/cancel
 */

import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'

/** The route owner is the Web carrier; the agent tool needs the tool registry. */
export const inject = ['webServer', 'tools']

const ROUTE = '/dsh-serial-console'
const MAX_CHUNKS = 800
const FLASH_TAIL = 300
const PORT_CACHE_MS = 30000
const MAX_BODY_BYTES = 64 * 1024

/** Agent-facing tool: a read-only window onto the panel's single serial reader. */
const TOOL_NAME = 'serial_console'
const TOOL_ACTIONS = ['status', 'read', 'ports', 'flash', 'connect', 'disconnect']
const TOOL_MAX_TEXT = 16000

const TOOL_DESCRIPTION = [
  'Read the serial console that lives in the GUI right sidebar (串口控制台).',
  'The panel and this tool share one reader, so a read never opens the COM port and never',
  'conflicts with the panel or with another serial monitor.',
  'Actions:',
  '- status (default): connection state, buffered chunk count, ports, and flash state.',
  '- read: device output. Pass `since` = the previous result buffer.nextSeq to get only new',
  '  data; `limit` caps how many chunks are returned (newest kept, 1-500, default 200).',
  '- ports: COM ports with their Windows device names.',
  '- flash: PlatformIO upload status, percentage, stage, and the tail of its log.',
  '- connect / disconnect: drive the panel\'s reader explicitly. `port` accepts "COM5" or',
  '  just "5" (listed or not); `baud` defaults to the panel rate. The panel follows this',
  '  connection, so the user always sees the port you opened.',
  'Start with status, then read, so the connection state is known before the log is interpreted.',
].join('\n')

const TOOL_PARAMETERS = {
  type: 'object',
  additionalProperties: false,
  properties: {
    action: {
      type: 'string',
      enum: TOOL_ACTIONS,
      description: 'status | read | ports | flash | connect | disconnect',
    },
    since: {
      type: 'integer',
      description: 'read: return only chunks after this sequence number (the previous result buffer.nextSeq).',
    },
    limit: {
      type: 'integer',
      description: 'read: maximum chunks to return, newest kept (1-500, default 200).',
    },
    port: {
      type: 'string',
      description: 'connect: port to open, "COM5" or just "5". Defaults to the current/panel selection.',
    },
    baud: {
      type: 'integer',
      description: 'connect: baud rate, e.g. 115200. Defaults to the current/panel rate.',
    },
  },
  required: ['action'],
}

const TOOL_OUTPUT_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  properties: {
    action: { type: 'string' },
    ok: { type: 'boolean' },
    message: { type: 'string' },
    serial: {
      type: 'object',
      additionalProperties: false,
      properties: {
        status: { type: 'string' },
        port: { type: 'string' },
        baud: { type: 'integer' },
        bytes: { type: 'integer' },
        chunks: { type: 'integer' },
        since: { type: 'integer' },
        lastDataAt: { type: 'integer' },
        error: { type: 'string' },
      },
      required: ['status', 'port', 'baud', 'bytes', 'chunks', 'since', 'lastDataAt', 'error'],
    },
    flash: {
      type: 'object',
      additionalProperties: false,
      properties: {
        status: { type: 'string' },
        phase: { type: 'string' },
        percent: { type: 'integer' },
        stage: { type: 'string' },
        env: { type: 'string' },
        projectDir: { type: 'string' },
        error: { type: 'string' },
        startedAt: { type: 'integer' },
        finishedAt: { type: 'integer' },
      },
      required: ['status', 'phase', 'percent', 'stage', 'env', 'projectDir', 'error', 'startedAt', 'finishedAt'],
    },
    ports: {
      type: 'array',
      items: {
        type: 'object',
        additionalProperties: false,
        properties: {
          path: { type: 'string' },
          name: { type: 'string' },
          status: { type: 'string' },
        },
        required: ['path', 'name', 'status'],
      },
    },
    oldestSeq: { type: 'integer' },
    nextSeq: { type: 'integer' },
    bufferedChunks: { type: 'integer' },
    text: { type: 'string' },
    truncated: { type: 'boolean' },
    projectDir: { type: 'string' },
    env: { type: 'string' },
  },
  required: [
    'action', 'ok', 'message', 'serial', 'flash', 'ports',
    'oldestSeq', 'nextSeq', 'bufferedChunks', 'text', 'truncated', 'projectDir', 'env',
  ],
}

/**
 * @param {unknown} error
 * @returns {string}
 */
function messageOf(error) {
  if (error instanceof Error) return error.message
  return String(error)
}

/** Absolute path of the always-present Windows PowerShell host. */
function powershellPath() {
  const root = process.env.SystemRoot
  if (root) {
    const candidate = join(root, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
    if (existsSync(candidate)) return candidate
  }
  return 'powershell.exe'
}

/** `-EncodedCommand` keeps quoting out of the argument list. */
function encodedCommand(script) {
  return Buffer.from(script, 'utf16le').toString('base64')
}

/** Strip the ANSI escapes PlatformIO and esptool use for colour. */
function stripAnsi(text) {
  return text.replace(/\u001b\[[0-9;?]*[ -/]*[@-~]/g, '')
}

/** Loopback-only route: the panel is a local tool, not a network service. */
function isLoopback(request) {
  const address = request.socket?.remoteAddress ?? ''
  return address === '127.0.0.1' || address === '::1' || address === '::ffff:127.0.0.1'
}

function sendJson(response, status, value) {
  const body = JSON.stringify(value)
  response.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store',
    'content-length': Buffer.byteLength(body),
  })
  response.end(body)
}

/** Read a JSON body, or an empty object when the caller sent none. */
async function readJson(request) {
  let size = 0
  const parts = []
  for await (const part of request) {
    size += part.length
    if (size > MAX_BODY_BYTES) throw new Error('request body too large')
    parts.push(part)
  }
  if (parts.length === 0) return {}
  const text = Buffer.concat(parts).toString('utf8').trim()
  if (!text) return {}
  const parsed = JSON.parse(text)
  return parsed !== null && typeof parsed === 'object' ? parsed : {}
}

/**
 * Read one COM port until the child is killed: raw bytes, framed as base64 lines
 * so binary payloads and missing newlines never corrupt the protocol.
 * @param {string} port - a validated `COM<n>` name.
 * @param {number} baud
 */
function serialScript(port, baud) {
  return `
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'
try {
  $sp = New-Object System.IO.Ports.SerialPort
  $sp.PortName = '${port}'
  $sp.BaudRate = ${baud}
  $sp.Parity = [System.IO.Ports.Parity]::None
  $sp.DataBits = 8
  $sp.StopBits = [System.IO.Ports.StopBits]::One
  $sp.ReadTimeout = 300
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  [Console]::Out.WriteLine('__OPEN__')
  [Console]::Out.Flush()
  $buffer = New-Object byte[] 1024
  while ($true) {
    try {
      $count = $sp.Read($buffer, 0, $buffer.Length)
    } catch [System.TimeoutException] {
      continue
    }
    if ($count -gt 0) {
      [Console]::Out.WriteLine('__DATA__' + [Convert]::ToBase64String($buffer, 0, $count))
      [Console]::Out.Flush()
    }
  }
} catch {
  [Console]::Out.WriteLine('__ERROR__' + $_.Exception.Message)
  [Console]::Out.Flush()
  exit 1
}
`
}

/**
 * Friendly names and plug status for the ports the registry already listed.
 * `Get-CimInstance` over all PnP devices costs about two seconds, so this runs
 * only in the background, never on a polling request.
 */
const PORT_NAMES_SCRIPT = `
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$devices = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue
$rows = foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
  $device = $devices | Where-Object { $_.Name -match [regex]::Escape("($p)") } | Select-Object -First 1
  [pscustomobject]@{
    path = $p
    name = $(if ($device) { $device.Name } else { $p })
    status = $(if ($device) { [string]$device.Status } else { '' })
  }
}
if ($null -eq $rows) { '[]' } else { $rows | ConvertTo-Json -Compress }
`

/**
 * Run a command and resolve its stdout on exit code 0.
 * @param {string} command
 * @param {string[]} args
 * @param {number} [timeoutMs]
 * @returns {Promise<string>}
 */
function runCapture(command, args, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    let child
    try {
      child = spawn(command, args, { windowsHide: true })
    } catch (error) {
      reject(error)
      return
    }
    let stdout = ''
    let stderr = ''
    let settled = false
    const timer = setTimeout(() => {
      if (settled) return
      settled = true
      try {
        child.kill()
      } catch {
        // already gone
      }
      reject(new Error(`${command} timed out`))
    }, timeoutMs)
    child.stdout.setEncoding('utf8')
    child.stderr.setEncoding('utf8')
    child.stdout.on('data', (chunk) => {
      stdout += chunk
    })
    child.stderr.on('data', (chunk) => {
      stderr += chunk
    })
    child.on('error', (error) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      reject(error)
    })
    child.on('exit', (code) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      if (code === 0) resolve(stdout)
      else reject(new Error(stderr.trim() || `${command} exited with code ${code}`))
    })
  })
}

/**
 * Run a short PowerShell script and return its stdout.
 * @param {string} script
 * @param {number} [timeoutMs]
 * @returns {Promise<string>}
 */
function runPowerShell(script, timeoutMs = 10000) {
  return runCapture(
    powershellPath(),
    ['-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', encodedCommand(script)],
    timeoutMs,
  )
}

/**
 * The COM ports Windows currently exposes. `SERIALCOMM` is a registry key, so
 * `reg query` answers in a few hundred milliseconds; PowerShell would cost two
 * seconds and is reserved for the optional device names.
 * @returns {Promise<string[]>}
 */
async function listPortPaths() {
  if (process.platform !== 'win32') return []
  try {
    const stdout = await runCapture('reg', ['query', 'HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM'], 5000)
    const paths = []
    for (const line of stdout.split(/\r?\n/)) {
      const match = /\sREG_SZ\s+(COM\d+)\s*$/i.exec(line)
      if (match) paths.push(match[1].toUpperCase())
    }
    return [...new Set(paths)].sort()
  } catch {
    // The key is absent when no serial device is attached.
    return []
  }
}

/**
 * Stop a child and everything it started. On Windows `pio.exe` runs SCons and the
 * compiler toolchain as grandchildren, so the tree has to be killed from the root
 * while the root is still alive; killing the root first only orphans them.
 * @param {import('node:child_process').ChildProcess} child
 */
function killTree(child) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return
  if (process.platform === 'win32' && child.pid) {
    try {
      const killer = spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' })
      killer.on('error', () => {
        try {
          child.kill()
        } catch {
          // already gone
        }
      })
      killer.on('exit', () => {
        if (child.exitCode === null && child.signalCode === null) {
          try {
            child.kill()
          } catch {
            // already gone
          }
        }
      })
      return
    } catch {
      // fall through to the plain kill
    }
  }
  try {
    child.kill()
  } catch {
    // already gone
  }
}

/**
 * @param {import('cordis').Context} ctx
 * @param {Record<string, unknown>} [config]
 */
export function apply(ctx, config = {}) {
  const options = {
    projectDir: '',
    env: 'esp32-s3-devkitm-1',
    baud: 115200,
    pioPath: '',
    ...(config ?? {}),
  }

  /** @type {{ seq: number, ts: number, text: string }[]} */
  let chunks = []
  let nextSeq = 1

  const serial = {
    status: 'disconnected',
    port: null,
    baud: Number(options.baud) || 115200,
    bytes: 0,
    chunks: 0,
    since: null,
    lastDataAt: null,
    error: null,
  }

  const flash = {
    status: 'idle',
    phase: 'idle',
    percent: 0,
    stage: '',
    env: String(options.env || ''),
    projectDir: '',
    startedAt: null,
    finishedAt: null,
    error: null,
    /** @type {string[]} */
    log: [],
  }

  /** @type {import('node:child_process').ChildProcess | null} */
  let serialChild = null
  /** @type {import('node:child_process').ChildProcess | null} */
  let flashChild = null
  let serialDecoder = new TextDecoder('utf-8')
  let serialStdout = ''
  let flashStdout = ''
  let flashStderr = ''

  function pushChunk(text) {
    if (!text) return
    const entry = { seq: nextSeq, ts: Date.now(), text }
    nextSeq += 1
    chunks.push(entry)
    if (chunks.length > MAX_CHUNKS) chunks = chunks.slice(chunks.length - MAX_CHUNKS)
    serial.chunks += 1
    serial.lastDataAt = entry.ts
  }

  /** @param {string} text */
  function pushFlashLine(text) {
    const line = stripAnsi(text).replace(/\s+$/, '')
    if (!line) return
    flash.log.push(line.length > 500 ? `${line.slice(0, 500)}…` : line)
    if (flash.log.length > FLASH_TAIL) flash.log = flash.log.slice(flash.log.length - FLASH_TAIL)
    interpretFlashLine(line)
  }

  /** Map one PlatformIO/esptool line onto the progress model. */
  function interpretFlashLine(line) {
    const percentMatch = /\((\d{1,3})\s*%\)/.exec(line)
    if (percentMatch) {
      const value = Math.max(0, Math.min(100, Number(percentMatch[1])))
      flash.percent = value
      flash.phase = 'upload'
      flash.stage = line.replace(/\(\d{1,3}\s*%\)/, '').trim() || flash.stage
      return
    }
    if (/^Processing\b/.test(line)) {
      flash.phase = 'prepare'
      flash.stage = `准备 ${flash.env}…`
      return
    }
    if (/^PLATFORM:/.test(line)) {
      flash.stage = '加载平台…'
      return
    }
    if (/^LDF:/.test(line)) {
      flash.stage = '解析依赖库…'
      return
    }
    if (/^Building in\b/.test(line)) {
      flash.phase = 'build'
      flash.stage = '编译中…'
      return
    }
    if (/^Compiling\b/.test(line)) {
      flash.phase = 'build'
      flash.stage = '编译中…'
      return
    }
    if (/^Linking\b/.test(line) || /^Archiving\b/.test(line)) {
      flash.phase = 'build'
      flash.stage = '链接固件…'
      return
    }
    if (/Configuring upload protocol/i.test(line)) {
      flash.phase = 'prepare'
      flash.stage = '准备上传…'
      return
    }
    if (/Looking for upload port/i.test(line)) {
      flash.phase = 'prepare'
      flash.stage = '查找串口…'
      return
    }
    if (/^Uploading \.pio\b/i.test(line)) {
      flash.phase = 'upload'
      flash.stage = '上传固件…'
      return
    }
    if (/^Connecting\.\.\./.test(line) || /^Connecting\b/.test(line)) {
      flash.phase = 'upload'
      flash.stage = '连接芯片…'
      return
    }
    if (/^Chip is\b/.test(line) || /^Detecting chip type/.test(line)) {
      flash.stage = line
      return
    }
    if (/Hash of data verified/i.test(line)) {
      flash.stage = '校验固件…'
      return
    }
    if (/Hard resetting|Leaving\.\.\./i.test(line)) {
      flash.phase = 'upload'
      flash.stage = '复位设备…'
      return
    }
    if (/^SUCCESS\b/.test(line) || /Took .* seconds/i.test(line)) {
      flash.stage = '完成'
    }
  }

  /** Last useful failure line, for the red status text. */
  function flashFailureReason(code) {
    const candidates = flash.log.filter((line) =>
      /(error|failed|cannot open|access is denied|A fatal error occurred|UnknownPackageError|not found)/i.test(line),
    )
    const last = candidates[candidates.length - 1]
    if (last) return last
    return `PlatformIO exited with code ${code}`
  }

  function resolveProjectDir() {
    if (typeof options.projectDir === 'string' && options.projectDir.trim()) return options.projectDir.trim()
    try {
      const workingDirectory = ctx.get('workingDirectory')
      const directory = workingDirectory?.defaultDirectory
      if (typeof directory === 'string' && directory) return directory
    } catch {
      // service absent in this composition
    }
    return process.cwd()
  }

  function resolvePio() {
    if (typeof options.pioPath === 'string' && options.pioPath.trim()) return options.pioPath.trim()
    const guess = join(homedir(), '.platformio', 'penv', 'Scripts', 'pio.exe')
    if (existsSync(guess)) return guess
    return 'pio'
  }

  let ports = []
  let portsAt = 0
  /** @type {Promise<unknown> | null} */
  let portsPending = null
  /** @type {Map<string, { name: string, status: string }>} */
  const portNames = new Map()
  let namesLoading = false

  /** Fill in the friendly device names after the fast list is already published. */
  async function enrichPortNames() {
    if (namesLoading) return
    namesLoading = true
    try {
      const stdout = (await runPowerShell(PORT_NAMES_SCRIPT, 20000)).trim()
      if (!stdout) return
      const parsed = JSON.parse(stdout)
      for (const row of Array.isArray(parsed) ? parsed : [parsed]) {
        if (!row || typeof row.path !== 'string') continue
        portNames.set(row.path.toUpperCase(), {
          name: typeof row.name === 'string' && row.name ? row.name : row.path,
          status: typeof row.status === 'string' ? row.status : '',
        })
      }
      ports = ports.map((entry) => {
        const known = portNames.get(entry.path)
        return known ? { ...entry, name: known.name, status: known.status } : entry
      })
    } catch {
      // Names are decoration; a failure keeps the plain port list.
    } finally {
      namesLoading = false
    }
  }

  async function refreshPorts(force = false) {
    if (!force && ports.length > 0 && Date.now() - portsAt < PORT_CACHE_MS) return ports
    if (portsPending) return portsPending
    portsPending = (async () => {
      try {
        const paths = await listPortPaths()
        ports = paths.map((path) => ({
          path,
          name: portNames.get(path)?.name ?? path,
          status: portNames.get(path)?.status ?? '',
        }))
        portsAt = Date.now()
        if (paths.length > 0 && paths.some((path) => !portNames.has(path))) void enrichPortNames()
        return ports
      } catch {
        // A failed enumeration must not break the panel: keep the last list.
        return ports
      } finally {
        portsPending = null
      }
    })()
    return portsPending
  }

  function stopSerial(reason) {
    const child = serialChild
    serialChild = null
    if (child) killTree(child)
    if (reason !== 'dispose') {
      serial.status = 'disconnected'
      serial.error = null
    }
    serialStdout = ''
    return child !== null
  }

  function connectSerial(port, baud) {
    const raw = String(port ?? '').trim().toUpperCase()
    // Accept a bare number too: "5" and "COM5" both mean the same port.
    const normalized = /^\d+$/.test(raw) ? `COM${raw}` : raw
    if (!/^COM\d+$/.test(normalized)) return { ok: false, error: '请选择有效的串口（COMx）' }
    const speed = Number(baud) || Number(options.baud) || 115200
    if (!Number.isFinite(speed) || speed <= 0) return { ok: false, error: '波特率无效' }

    stopSerial('reconnect')
    serialDecoder = new TextDecoder('utf-8')
    serialStdout = ''
    serial.status = 'connecting'
    serial.port = normalized
    serial.baud = speed
    serial.bytes = 0
    serial.chunks = 0
    serial.since = Date.now()
    serial.lastDataAt = null
    serial.error = null

    let child
    try {
      child = spawn(
        powershellPath(),
        [
          '-NoLogo',
          '-NoProfile',
          '-NonInteractive',
          '-ExecutionPolicy',
          'Bypass',
          '-EncodedCommand',
          encodedCommand(serialScript(normalized, speed)),
        ],
        { windowsHide: true },
      )
    } catch (error) {
      serial.status = 'error'
      serial.error = messageOf(error)
      return { ok: false, error: serial.error }
    }

    serialChild = child
    let reportedError = null

    child.stdout.setEncoding('utf8')
    child.stdout.on('data', (chunk) => {
      serialStdout += chunk
      const lines = serialStdout.split(/\r?\n/)
      serialStdout = lines.pop() ?? ''
      for (const line of lines) {
        if (!line) continue
        if (line === '__OPEN__') {
          serial.status = 'connected'
          serial.error = null
          continue
        }
        if (line.startsWith('__DATA__')) {
          try {
            const bytes = Buffer.from(line.slice('__DATA__'.length), 'base64')
            serial.bytes += bytes.length
            const text = serialDecoder.decode(bytes, { stream: true })
            pushChunk(text)
          } catch {
            // ignore a corrupt frame
          }
          continue
        }
        if (line.startsWith('__ERROR__')) {
          reportedError = line.slice('__ERROR__'.length).trim() || '串口打开失败'
          serial.status = 'error'
          serial.error = reportedError
        }
      }
    })
    child.stderr.setEncoding('utf8')
    child.stderr.on('data', () => {})
    child.on('error', (error) => {
      if (serialChild === child) serialChild = null
      serial.status = 'error'
      serial.error = messageOf(error)
    })
    child.on('exit', (code) => {
      if (serialChild === child) serialChild = null
      if (serial.status === 'disconnected') return
      serial.status = 'error'
      serial.error = reportedError ?? `串口进程已退出（代码 ${code}）`
    })

    return { ok: true }
  }

  function stopFlash(reason) {
    const child = flashChild
    flashChild = null
    if (child) killTree(child)
    if (reason === 'cancel' && flash.status === 'running') {
      flash.status = 'cancelled'
      flash.finishedAt = Date.now()
      flash.stage = '已取消'
      pushFlashLine('[已取消]')
    }
  }

  function startFlash(request) {
    if (flashChild) return { ok: false, error: '烧录已在进行中' }

    const environment = String(request?.env || options.env || '').trim()
    if (!/^[A-Za-z0-9_.-]+$/.test(environment)) return { ok: false, error: 'PlatformIO 环境名无效' }

    const directory = resolveProjectDir()
    if (!existsSync(join(directory, 'platformio.ini'))) {
      return { ok: false, error: `目录中没有 platformio.ini：${directory}` }
    }

    const hadSerial = serialChild !== null
    if (hadSerial) stopSerial('flash')

    flash.status = 'running'
    flash.phase = 'start'
    flash.percent = 0
    flash.stage = '启动 PlatformIO…'
    flash.env = environment
    flash.projectDir = directory
    flash.startedAt = Date.now()
    flash.finishedAt = null
    flash.error = null
    flash.log = []
    flashStdout = ''
    flashStderr = ''

    if (hadSerial) pushFlashLine('[串口已断开：烧录需要独占端口]')

    const args = ['run', '-e', environment, '-t', 'upload']
    const uploadPort = String(request?.port ?? '').trim()
    if (/^COM\d+$/i.test(uploadPort)) args.push('--upload-port', uploadPort.toUpperCase())

    let child
    try {
      child = spawn(resolvePio(), args, {
        cwd: directory,
        env: { ...process.env, PYTHONUNBUFFERED: '1', PYTHONIOENCODING: 'utf-8' },
        windowsHide: true,
      })
    } catch (error) {
      flash.status = 'failed'
      flash.error = messageOf(error)
      flash.finishedAt = Date.now()
      flash.stage = '无法启动 PlatformIO'
      return { ok: false, error: flash.error }
    }

    flashChild = child
    let spawnFailed = null

    child.stdout.setEncoding('utf8')
    child.stderr.setEncoding('utf8')
    child.stdout.on('data', (chunk) => {
      flashStdout += chunk
      const lines = flashStdout.split(/\r?\n/)
      flashStdout = lines.pop() ?? ''
      for (const line of lines) pushFlashLine(line)
    })
    child.stderr.on('data', (chunk) => {
      flashStderr += chunk
      const lines = flashStderr.split(/\r?\n/)
      flashStderr = lines.pop() ?? ''
      for (const line of lines) pushFlashLine(line)
    })
    child.on('error', (error) => {
      spawnFailed = messageOf(error)
      if (flashChild === child) flashChild = null
      flash.status = 'failed'
      flash.error = spawnFailed
      flash.stage = '无法启动 PlatformIO'
      flash.finishedAt = Date.now()
    })
    child.on('exit', (code) => {
      if (flashChild === child) flashChild = null
      if (flashStdout) {
        pushFlashLine(flashStdout)
        flashStdout = ''
      }
      if (flashStderr) {
        pushFlashLine(flashStderr)
        flashStderr = ''
      }
      if (flash.status === 'cancelled') {
        flash.finishedAt = Date.now()
        return
      }
      if (spawnFailed) return
      flash.finishedAt = Date.now()
      if (code === 0) {
        flash.status = 'success'
        flash.phase = 'done'
        flash.percent = 100
        flash.stage = '烧录完成'
      } else {
        flash.status = 'failed'
        flash.error = flashFailureReason(code)
        flash.stage = '烧录失败'
      }
    })

    return { ok: true }
  }

  function snapshot(since) {
    const cursor = Number.isFinite(since) ? since : 0
    const lines = chunks.filter((entry) => entry.seq > cursor)
    return {
      now: Date.now(),
      serial: { ...serial },
      flash: {
        ...flash,
        log: flash.log.slice(-FLASH_TAIL),
        projectDir: flash.projectDir || resolveProjectDir(),
        env: flash.env || String(options.env || ''),
      },
      ports,
      lines,
      nextSeq,
      oldestSeq: chunks.length > 0 ? chunks[0].seq : nextSeq,
      projectDir: resolveProjectDir(),
      env: String(options.env || ''),
      pio: resolvePio(),
    }
  }

  /** Wait until the just-started reader reports open or failed, so callers get a settled state. */
  function waitForSerialSettled(timeoutMs = 1500) {
    return new Promise((resolve) => {
      const started = Date.now()
      const timer = setInterval(() => {
        if (serial.status !== 'connecting' || Date.now() - started > timeoutMs) {
          clearInterval(timer)
          resolve()
        }
      }, 50)
    })
  }

  // ------------------------------------------------------- agent-facing tool

  function toolSerialView() {
    return {
      status: serial.status,
      port: serial.port ?? '',
      baud: serial.baud,
      bytes: serial.bytes,
      chunks: serial.chunks,
      since: serial.since ?? 0,
      lastDataAt: serial.lastDataAt ?? 0,
      error: serial.error ?? '',
    }
  }

  function toolFlashView() {
    return {
      status: flash.status,
      phase: flash.phase,
      percent: flash.percent,
      stage: flash.stage,
      env: flash.env || String(options.env || ''),
      projectDir: flash.projectDir || resolveProjectDir(),
      error: flash.error ?? '',
      startedAt: flash.startedAt ?? 0,
      finishedAt: flash.finishedAt ?? 0,
    }
  }

  /** One shape for every action, so the output schema stays exact. */
  function toolResult(action, extra = {}) {
    return {
      action,
      ok: extra.ok !== false,
      message: typeof extra.message === 'string' ? extra.message : '',
      serial: toolSerialView(),
      flash: toolFlashView(),
      ports: ports.map((entry) => ({ path: entry.path, name: entry.name, status: entry.status })),
      oldestSeq: chunks.length > 0 ? chunks[0].seq : nextSeq,
      nextSeq,
      bufferedChunks: chunks.length,
      text: typeof extra.text === 'string' ? extra.text : '',
      truncated: extra.truncated === true,
      projectDir: resolveProjectDir(),
      env: String(options.env || ''),
    }
  }

  function renderToolResult(value) {
    const status = value.serial
    const line = [
      `Serial: ${status.status}${status.port ? ` ${status.port} @ ${status.baud}` : ''}` +
        ` · ${status.bytes} B · ${status.chunks} chunks` +
        (status.lastDataAt > 0 ? ` · last data ${new Date(status.lastDataAt).toLocaleTimeString()}` : ''),
    ]
    if (status.error) line.push(`Serial error: ${status.error}`)
    line.push(`Buffer: seq ${value.oldestSeq}..${value.nextSeq} (${value.bufferedChunks} chunks)`)
    line.push(
      `Flash: ${value.flash.status} · ${value.flash.percent}% · ${value.flash.stage || '-'}` +
        (value.flash.error ? ` · ${value.flash.error}` : ''),
    )
    line.push(`Project: ${value.projectDir} (env ${value.env})`)
    line.push(
      value.ports.length === 0
        ? 'Ports: none'
        : `Ports: ${value.ports
            .map(
              (entry) =>
                `${entry.path}${entry.name && entry.name !== entry.path ? ` (${entry.name})` : ''}` +
                `${entry.status ? ` [${entry.status}]` : ''}`,
            )
            .join(', ')}`,
    )
    if (value.message) line.push(`Message: ${value.message}`)
    if (value.text) {
      line.push(`Output${value.truncated ? ' (truncated to the newest chunks)' : ''}:`)
      line.push(value.text)
    } else if (value.action === 'read') {
      line.push('Output: (empty)')
    }
    return line.join('\n')
  }

  async function executeSerialTool(rawArgs) {
    const args = rawArgs !== null && typeof rawArgs === 'object' ? rawArgs : {}
    const action = TOOL_ACTIONS.includes(args.action) ? args.action : 'status'

    if (action === 'ports') {
      await refreshPorts(true)
      return toolResult('ports')
    }

    if (action === 'connect') {
      const port =
        typeof args.port === 'string' && args.port.trim()
          ? args.port.trim()
          : serial.port || ports[0]?.path || ''
      const baud = Number(args.baud) || serial.baud || Number(options.baud) || 115200
      const outcome = connectSerial(port, baud)
      if (outcome.ok) await waitForSerialSettled()
      if (outcome.ok) await refreshPorts(true)
      return toolResult('connect', {
        ok: outcome.ok !== false && serial.status === 'connected',
        message:
          outcome.ok === false
            ? outcome.error
            : serial.status === 'connected'
              ? `connected ${serial.port ?? port} @ ${baud}`
              : serial.error ?? 'the port did not open',
      })
    }

    if (action === 'disconnect') {
      const had = stopSerial('user')
      return toolResult('disconnect', { message: had ? 'disconnected' : 'no port was open' })
    }

    if (action === 'flash') {
      return toolResult('flash', { text: flash.log.slice(-60).join('\n') })
    }

    if (action === 'read') {
      const limit = Math.min(500, Math.max(1, Number(args.limit) || 200))
      const since = Number(args.since) || 0
      const selected = chunks.filter((entry) => entry.seq > since)
      const kept = selected.slice(Math.max(0, selected.length - limit))
      let text = ''
      let truncated = selected.length > kept.length
      for (const chunk of kept) {
        if (text.length >= TOOL_MAX_TEXT) {
          truncated = true
          break
        }
        text += chunk.text
      }
      if (text.length > TOOL_MAX_TEXT) {
        text = text.slice(0, TOOL_MAX_TEXT)
        truncated = true
      }
      return toolResult('read', {
        text,
        truncated,
        message:
          serial.status === 'connected' || chunks.length > 0
            ? ''
            : 'the panel is not connected; open the right sidebar or call action=connect first',
      })
    }

    await refreshPorts(false)
    return toolResult('status')
  }

  ctx.effect(
    () =>
      ctx.tools.register({
        name: TOOL_NAME,
        description: TOOL_DESCRIPTION,
        parameters: TOOL_PARAMETERS,
        output: {
          schema: TOOL_OUTPUT_SCHEMA,
          render: (_args, value) => [{ type: 'text', text: renderToolResult(value) }],
        },
        isConcurrencySafe: (args) => !args || (args.action !== 'connect' && args.action !== 'disconnect'),
        execute: executeSerialTool,
      }),
    'serial-console: serial_console tool',
  )

  const route = {
    kind: 'prefix',
    path: ROUTE,
    handler: async (request, response) => {
      if (!isLoopback(request)) {
        sendJson(response, 403, { error: '仅允许本机访问' })
        return
      }
      try {
        const url = new URL(request.url ?? '/', 'http://127.0.0.1')
        const path = url.pathname.slice(ROUTE.length).replace(/\/+$/, '') || '/'

        if (request.method === 'GET' && (path === '/state' || path === '/')) {
          const force = url.searchParams.get('ports') === '1'
          await refreshPorts(force)
          const since = Number(url.searchParams.get('since') ?? '0')
          sendJson(response, 200, snapshot(since))
          return
        }

        if (request.method === 'POST') {
          const body = await readJson(request)
          if (path === '/connect') {
            const outcome = connectSerial(body.port, body.baud)
            if (outcome.ok) await waitForSerialSettled()
            sendJson(
              response,
              200,
              outcome.ok === false
                ? outcome
                : { ok: serial.status === 'connected', error: serial.error ?? '' },
            )
            return
          }
          if (path === '/disconnect') {
            const had = stopSerial('user')
            sendJson(response, 200, { ok: true, had })
            return
          }
          if (path === '/clear') {
            chunks = []
            sendJson(response, 200, { ok: true })
            return
          }
          if (path === '/flash') {
            sendJson(response, 200, startFlash(body))
            return
          }
          if (path === '/flash/cancel') {
            stopFlash('cancel')
            sendJson(response, 200, { ok: true })
            return
          }
        }

        sendJson(response, 404, { error: '未知的接口' })
      } catch (error) {
        sendJson(response, 500, { error: messageOf(error) })
      }
    },
  }

  ctx.effect(() => ctx.webServer.register(route), 'serial-console: loopback route')

  ctx.effect(
    () => () => {
      stopSerial('dispose')
      if (flashChild) {
        stopFlash('dispose')
        flashChild = null
      }
    },
    'serial-console: child processes',
  )
}
