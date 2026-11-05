/**
 * Browser half of the serial-console bundle.
 *
 * Registers one right-sidebar tab type (plus its guide entry, the entry point a
 * person sees when the right column is open) and renders three answers in it:
 * live serial output, the COM-port connection status, and PlatformIO flash
 * progress. All host work reaches the page over the bundle's loopback route.
 */

window.__ModuleLoader__.load({
  id: '@local/dsh-serial-console',
  factory(require) {
    const React = require('react')
    const h = React.createElement
    const NS = 'serialConsole'
    const ROUTE = '/dsh-serial-console'
    const TYPE_ID = '@local/dsh-serial-console'

    const zh = {
      tabTitle: '串口控制台',
      guideTitle: '串口控制台',
      guideDescription: '串口数据、连接状态与烧录进度',
      serialTab: '串口数据',
      flashTab: '烧录',
      port: '端口',
      baud: '波特率',
      refresh: '刷新',
      connect: '连接',
      disconnect: '断开',
      clear: '清空',
      autoScroll: '自动滚动',
      pause: '暂停',
      resume: '继续',
      pausedHint: '已暂停显示（数据仍在后台接收）',
      noPorts: '未检测到串口设备',
      emptySerial: '连接串口后，设备的输出会显示在这里。',
      statusDisconnected: '未连接',
      statusConnecting: '正在打开串口…',
      statusConnected: '已连接',
      statusError: '连接异常',
      received: '已接收 {bytes}',
      chunks: '{count} 块',
      connectedFor: '已连接 {time}',
      flashIdle: '未开始',
      flashRunning: '烧录中',
      flashSuccess: '烧录成功',
      flashFailed: '烧录失败',
      flashCancelled: '已取消',
      flashStart: '开始烧录',
      flashStop: '停止',
      flashEmpty: '点击“开始烧录”运行 PlatformIO 上传，进度和日志会显示在这里。',
      env: '环境',
      project: '工程',
      backendDown: '无法连接插件后端：{message}',
      waitingData: '等待设备数据…',
    }

    const en = {
      tabTitle: 'Serial',
      guideTitle: 'Serial console',
      guideDescription: 'Serial data, port status and flash progress',
      serialTab: 'Serial data',
      flashTab: 'Flash',
      port: 'Port',
      baud: 'Baud',
      refresh: 'Refresh',
      connect: 'Connect',
      disconnect: 'Disconnect',
      clear: 'Clear',
      autoScroll: 'Auto-scroll',
      pause: 'Pause',
      resume: 'Resume',
      pausedHint: 'Display paused (data is still arriving)',
      noPorts: 'No serial device found',
      emptySerial: 'Connect a port and the device output appears here.',
      statusDisconnected: 'Disconnected',
      statusConnecting: 'Opening port…',
      statusConnected: 'Connected',
      statusError: 'Connection error',
      received: 'Received {bytes}',
      chunks: '{count} blocks',
      connectedFor: 'Connected for {time}',
      flashIdle: 'Not started',
      flashRunning: 'Flashing',
      flashSuccess: 'Flash succeeded',
      flashFailed: 'Flash failed',
      flashCancelled: 'Cancelled',
      flashStart: 'Start flash',
      flashStop: 'Stop',
      flashEmpty: 'Start a PlatformIO upload and its progress and log appear here.',
      env: 'Environment',
      project: 'Project',
      backendDown: 'Plugin backend unreachable: {message}',
      waitingData: 'Waiting for device data…',
    }

    const CSS = `
.dsc-root{display:flex;flex-direction:column;gap:10px;height:100%;min-height:0;padding:12px;box-sizing:border-box;color:var(--dsw-alias-label-primary);font-size:13px;line-height:1.45}
.dsc-card{display:flex;flex-direction:column;gap:8px;padding:10px;border:1px solid var(--dsw-alias-border-l1);border-radius:10px;background:var(--dsw-alias-bg-layer-1);flex:none}
.dsc-row{display:flex;align-items:center;gap:8px;min-width:0}
.dsc-grow{flex:1;min-width:0}
.dsc-dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--dsw-alias-state-idle-primary)}
.dsc-dot--on{background:var(--dsw-alias-state-success-primary)}
.dsc-dot--warn{background:var(--dsw-alias-state-warn-primary)}
.dsc-dot--error{background:var(--dsw-alias-state-error-primary)}
.dsc-strong{font-weight:600}
.dsc-muted{color:var(--dsw-alias-label-secondary);font-size:12px}
.dsc-ellipsis{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.dsc-mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace}
.dsc-port,.dsc-input{height:28px;border-radius:7px;border:1px solid var(--dsw-alias-border-l2);background:var(--dsw-alias-bg-base);color:var(--dsw-alias-label-primary);font:inherit;padding:0 8px;min-width:0;box-sizing:border-box}
.dsc-port{flex:1;text-transform:uppercase}
.dsc-port:disabled{opacity:.75}
.dsc-input{width:82px}
.dsc-btn{height:28px;padding:0 12px;border-radius:7px;border:1px solid var(--dsw-alias-border-l2);background:var(--dsw-alias-bg-layer-2);color:var(--dsw-alias-label-primary);font:inherit;cursor:pointer;white-space:nowrap;flex:none}
.dsc-btn:hover:not(:disabled){border-color:var(--dsw-alias-border-l2);background:var(--dsw-alias-bg-layer-1)}
.dsc-btn:disabled{opacity:.5;cursor:default}
.dsc-btn--primary{border-color:var(--dsw-alias-brand-primary);color:var(--dsw-alias-brand-primary)}
.dsc-tabs{display:flex;gap:14px;border-bottom:1px solid var(--dsw-alias-border-l1);flex:none}
.dsc-tab{appearance:none;background:none;border:none;border-bottom:2px solid transparent;margin-bottom:-1px;padding:4px 0 6px;font:inherit;color:var(--dsw-alias-label-secondary);cursor:pointer}
.dsc-tab--active{color:var(--dsw-alias-label-primary);border-bottom-color:var(--dsw-alias-brand-primary)}
.dsc-pane{display:flex;flex-direction:column;gap:8px;flex:1;min-height:0}
.dsc-toolbar{display:flex;align-items:center;gap:8px;flex:none;flex-wrap:wrap}
.dsc-check{display:flex;align-items:center;gap:5px;color:var(--dsw-alias-label-secondary);font-size:12px;cursor:pointer;user-select:none}
.dsc-log{flex:1;min-height:140px;margin:0;padding:8px 10px;overflow:auto;border:1px solid var(--dsw-alias-border-l1);border-radius:9px;background:var(--dsw-alias-bg-base);color:var(--dsw-alias-label-primary);font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace;font-size:12px;line-height:1.5;white-space:pre-wrap;word-break:break-word;tab-size:4}
.dsc-placeholder{flex:1;display:flex;align-items:center;justify-content:center;text-align:center;padding:18px;border:1px dashed var(--dsw-alias-border-l1);border-radius:9px;color:var(--dsw-alias-label-secondary);font-size:12px}
.dsc-bar{height:6px;border-radius:99px;background:var(--dsw-alias-bg-layer-2);overflow:hidden;flex:none}
.dsc-bar-fill{height:100%;background:var(--dsw-alias-brand-primary);transition:width .25s ease}
.dsc-notice{color:var(--dsw-alias-state-error-primary);font-size:12px;flex:none}
.dsc-flash-head{display:flex;align-items:center;gap:8px}
.dsc-percent{margin-left:auto;font-variant-numeric:tabular-nums;font-weight:600}
`

    // ---------------------------------------------------------------- icons

    function SerialIcon({ size, className }) {
      const s = typeof size === 'number' ? size : 16
      return h(
        'svg',
        {
          width: s,
          height: s,
          viewBox: '0 0 16 16',
          fill: 'none',
          stroke: 'currentColor',
          strokeWidth: 1.3,
          strokeLinecap: 'round',
          strokeLinejoin: 'round',
          className,
          'aria-hidden': true,
        },
        h('path', { d: 'M2 3.2h2.6v9.6H2z' }),
        h('path', { d: 'M4.6 8h2.2l1.6-2.6h2.1' }),
        h('path', { d: 'M4.6 8h2.2l1.6 2.6h2.1' }),
        h('path', { d: 'M12.3 4.1h1.4M12.3 11.9h1.4' }),
      )
    }

    function TabTitle() {
      return h(
        'span',
        { style: { display: 'inline-flex', alignItems: 'center', gap: '6px', minWidth: 0 } },
        h(SerialIcon, { size: 14 }),
        h('span', { className: 'dsc-ellipsis' }, t('tabTitle')),
      )
    }

    // -------------------------------------------------------------- helpers

    function formatBytes(value) {
      const n = Number(value) || 0
      if (n < 1024) return `${n} B`
      if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
      return `${(n / (1024 * 1024)).toFixed(2)} MB`
    }

    function formatDuration(ms) {
      const total = Math.max(0, Math.floor(ms / 1000))
      const minutes = Math.floor(total / 60)
      const seconds = total % 60
      return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`
    }

    // ---------------------------------------------------------- body view

    function SerialConsoleBody(props) {
      const tabInfo = typeof props.useTabInfo === 'function' ? props.useTabInfo() : null
      const visible = tabInfo?.tab?.visible !== false

      const [snapshot, setSnapshot] = React.useState(null)
      const [transportError, setTransportError] = React.useState(null)
      const [notice, setNotice] = React.useState(null)
      const [text, setText] = React.useState('')
      const [tab, setTab] = React.useState('serial')
      const [autoScroll, setAutoScroll] = React.useState(true)
      const [paused, setPaused] = React.useState(false)
      const [portChoice, setPortChoice] = React.useState('')
      const [baudText, setBaudText] = React.useState('115200')
      const [busy, setBusy] = React.useState(false)
      const [refreshKey, setRefreshKey] = React.useState(0)
      const [, setClock] = React.useState(0)

      const cursorRef = React.useRef(0)
      const bufferRef = React.useRef('')
      const pausedRef = React.useRef(false)
      const scrollRef = React.useRef(null)
      const initialisedRef = React.useRef(false)
      const flashRef = React.useRef(null)

      pausedRef.current = paused

      // Poll one state document per tick; no overlapping requests.
      React.useEffect(() => {
        if (!visible) return undefined
        let stopped = false
        let timer = null
        let ports = !initialisedRef.current || refreshKey > 0
        const tick = async () => {
          try {
            const withPorts = ports
            ports = false
            const response = await fetch(`${ROUTE}/state?since=${cursorRef.current}${withPorts ? '&ports=1' : ''}`, {
              cache: 'no-store',
              headers: { accept: 'application/json' },
            })
            if (!response.ok) throw new Error(`HTTP ${response.status}`)
            const data = await response.json()
            if (stopped) return
            setTransportError(null)
            setSnapshot(data)

            if (!initialisedRef.current) {
              initialisedRef.current = true
              if (typeof data.serial?.baud === 'number') setBaudText(String(data.serial.baud))
              const available = Array.isArray(data.ports) ? data.ports : []
              setPortChoice((current) => current || data.serial?.port || available[0]?.path || '')
            }

            if (typeof data.nextSeq === 'number') {
              if (data.nextSeq < cursorRef.current) {
                cursorRef.current = 0
                bufferRef.current = ''
                setText('')
              } else {
                cursorRef.current = data.nextSeq
              }
            }

            const incoming = Array.isArray(data.lines) ? data.lines : []
            if (incoming.length > 0) {
              let next = bufferRef.current
              for (const line of incoming) {
                if (line && typeof line.text === 'string') next += line.text
              }
              if (next.length > 200000) next = next.slice(next.length - 200000)
              bufferRef.current = next
              if (!pausedRef.current) setText(next)
            }

            const portsNow = Array.isArray(data.ports) ? data.ports : []
            const livePort = typeof data.serial?.port === 'string' ? data.serial.port : ''
            const opening = data.serial?.status === 'connected' || data.serial?.status === 'connecting'
            setPortChoice((current) => {
              if (opening && livePort) return livePort
              if (current) return current
              if (livePort) return livePort
              return portsNow[0]?.path || ''
            })
          } catch (error) {
            if (stopped) return
            setTransportError(error instanceof Error ? error.message : String(error))
          }
          if (!stopped) timer = setTimeout(tick, 700)
        }
        tick()
        return () => {
          stopped = true
          if (timer) clearTimeout(timer)
        }
      }, [visible, refreshKey])

      // A second-granularity clock keeps the connection duration live.
      React.useEffect(() => {
        const handle = setInterval(() => setClock((value) => value + 1), 1000)
        return () => clearInterval(handle)
      }, [])

      React.useEffect(() => {
        if (!autoScroll) return
        const element = scrollRef.current
        if (element) element.scrollTop = element.scrollHeight
      }, [text, autoScroll, tab])

      flashRef.current = snapshot?.flash ?? null
      React.useEffect(() => {
        if (tab !== 'flash') return
        const element = scrollRef.current
        if (element) element.scrollTop = element.scrollHeight
      }, [snapshot?.flash?.log?.length, tab])

      const request = React.useCallback(async (path, body) => {
        setBusy(true)
        setNotice(null)
        try {
          const response = await fetch(`${ROUTE}${path}`, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify(body ?? {}),
          })
          const data = await response.json().catch(() => ({}))
          if (!response.ok || data.ok === false) {
            setNotice(data.error || `HTTP ${response.status}`)
            return null
          }
          return data
        } catch (error) {
          setNotice(error instanceof Error ? error.message : String(error))
          return null
        } finally {
          setBusy(false)
        }
      }, [])

      const serial = snapshot?.serial ?? {
        status: 'disconnected',
        port: null,
        baud: 115200,
        bytes: 0,
        chunks: 0,
        since: null,
        lastDataAt: null,
        error: null,
      }
      const flash = snapshot?.flash ?? {
        status: 'idle',
        phase: 'idle',
        percent: 0,
        stage: '',
        env: '',
        projectDir: '',
        log: [],
        error: null,
      }
      const ports = Array.isArray(snapshot?.ports) ? snapshot.ports : []
      const connected = serial.status === 'connected' || serial.status === 'connecting'
      const flashRunning = flash.status === 'running'
      const now = snapshot?.now ?? Date.now()

      const statusLabel =
        serial.status === 'connected'
          ? t('statusConnected')
          : serial.status === 'connecting'
            ? t('statusConnecting')
            : serial.status === 'error'
              ? t('statusError')
              : t('statusDisconnected')
      const dotClass =
        serial.status === 'connected'
          ? 'dsc-dot dsc-dot--on'
          : serial.status === 'connecting'
            ? 'dsc-dot dsc-dot--warn'
            : serial.status === 'error'
              ? 'dsc-dot dsc-dot--error'
              : 'dsc-dot'

      const flashLabel =
        flash.status === 'running'
          ? t('flashRunning')
          : flash.status === 'success'
            ? t('flashSuccess')
            : flash.status === 'failed'
              ? t('flashFailed')
              : flash.status === 'cancelled'
                ? t('flashCancelled')
                : t('flashIdle')
      const flashDot =
        flash.status === 'running'
          ? 'dsc-dot dsc-dot--warn'
          : flash.status === 'success'
            ? 'dsc-dot dsc-dot--on'
            : flash.status === 'failed'
              ? 'dsc-dot dsc-dot--error'
              : 'dsc-dot'

      const portSuggestions = ports.map((entry) =>
        h('option', { key: entry.path, value: entry.path }, entry.name && entry.name !== entry.path ? entry.name : entry.path),
      )
      const portValue = connected && serial.port ? serial.port : portChoice
      const portListId = React.useMemo(() => `dsc-port-list-${Math.random().toString(36).slice(2, 8)}`, [])
      const statusCard = h(
        'section',
        { className: 'dsc-card', key: 'status' },
        h(
          'div',
          { className: 'dsc-row' },
          h('span', { className: dotClass, 'aria-hidden': true }),
          h('span', { className: 'dsc-strong' }, statusLabel),
          h('span', { className: 'dsc-muted dsc-ellipsis dsc-grow' }, connected ? `${serial.port} @ ${serial.baud}` : ''),
          h(
            'button',
            {
              type: 'button',
              className: 'dsc-btn',
              style: { marginLeft: 'auto', height: '22px', padding: '0 8px', fontSize: '11px' },
              disabled: busy,
              onClick: () => setRefreshKey((value) => value + 1),
            },
            t('refresh'),
          ),
        ),
        h(
          'div',
          { className: 'dsc-row' },
          h('input', {
            className: 'dsc-port dsc-mono',
            'aria-label': t('port'),
            list: portListId,
            value: portValue,
            placeholder: 'COM5',
            disabled: connected || busy,
            spellCheck: false,
            autoComplete: 'off',
            onChange: (event) => setPortChoice(event.target.value.toUpperCase()),
          }),
          h('datalist', { id: portListId }, ...portSuggestions),
          h('input', {
            className: 'dsc-input dsc-mono',
            'aria-label': t('baud'),
            value: baudText,
            disabled: connected,
            inputMode: 'numeric',
            onChange: (event) => setBaudText(event.target.value.replace(/[^0-9]/g, '').slice(0, 7)),
          }),
          h(
            'button',
            {
              type: 'button',
              className: 'dsc-btn',
              disabled: busy,
              onClick: () => request('/connect', { port: portChoice, baud: Number(baudText) || 115200 }),
              hidden: connected,
            },
            t('connect'),
          ),
          h(
            'button',
            {
              type: 'button',
              className: 'dsc-btn',
              disabled: busy,
              onClick: () => request('/disconnect', {}),
              hidden: !connected,
            },
            t('disconnect'),
          ),
        ),
        h(
          'div',
          { className: 'dsc-row dsc-muted' },
          h('span', null, t('received', { bytes: formatBytes(serial.bytes) })),
          h('span', null, '·'),
          h('span', null, t('chunks', { count: serial.chunks })),
          serial.since ? h('span', null, '·') : null,
          serial.since ? h('span', null, t('connectedFor', { time: formatDuration(now - serial.since) })) : null,
        ),
        serial.status === 'error' && serial.error
          ? h('div', { className: 'dsc-notice' }, serial.error)
          : null,
        notice ? h('div', { className: 'dsc-notice' }, notice) : null,
      )

      const tabsRow = h(
        'div',
        { className: 'dsc-tabs', key: 'tabs' },
        h(
          'button',
          {
            type: 'button',
            className: `dsc-tab${tab === 'serial' ? ' dsc-tab--active' : ''}`,
            onClick: () => setTab('serial'),
          },
          t('serialTab'),
        ),
        h(
          'button',
          {
            type: 'button',
            className: `dsc-tab${tab === 'flash' ? ' dsc-tab--active' : ''}`,
            onClick: () => setTab('flash'),
          },
          t('flashTab'),
          flashRunning ? h('span', { className: 'dsc-muted' }, ` ${flash.percent}%`) : null,
        ),
      )

      let pane
      if (tab === 'serial') {
        pane = h(
          'div',
          { className: 'dsc-pane', key: 'serial-pane' },
          h(
            'div',
            { className: 'dsc-toolbar' },
            h(
              'button',
              {
                type: 'button',
                className: 'dsc-btn',
                disabled: busy,
                onClick: () => {
                  bufferRef.current = ''
                  setText('')
                  request('/clear', {})
                },
              },
              t('clear'),
            ),
            h(
              'button',
              {
                type: 'button',
                className: 'dsc-btn',
                onClick: () => {
                  const next = !paused
                  setPaused(next)
                  if (!next) setText(bufferRef.current)
                },
              },
              paused ? t('resume') : t('pause'),
            ),
            h(
              'label',
              { className: 'dsc-check' },
              h('input', {
                type: 'checkbox',
                checked: autoScroll,
                onChange: (event) => setAutoScroll(event.target.checked),
              }),
              t('autoScroll'),
            ),
            paused ? h('span', { className: 'dsc-muted' }, t('pausedHint')) : null,
          ),
          text
            ? h('pre', { className: 'dsc-log', ref: scrollRef, tabIndex: -1 }, text)
            : h(
                'div',
                { className: 'dsc-placeholder' },
                connected ? t('waitingData') : t('emptySerial'),
              ),
        )
      } else {
        pane = h(
          'div',
          { className: 'dsc-pane', key: 'flash-pane' },
          h(
            'div',
            { className: 'dsc-card' },
            h(
              'div',
              { className: 'dsc-flash-head' },
              h('span', { className: flashDot, 'aria-hidden': true }),
              h('span', { className: 'dsc-strong' }, flashLabel),
              h('span', { className: 'dsc-percent' }, `${flashRunning || flash.percent > 0 ? flash.percent : 0}%`),
            ),
            h(
              'div',
              { className: 'dsc-bar' },
              h('div', { className: 'dsc-bar-fill', style: { width: `${Math.max(0, Math.min(100, flash.percent || 0))}%` } }),
            ),
            h('div', { className: 'dsc-muted dsc-ellipsis', title: flash.stage || '' }, flash.stage || ' '),
            h(
              'div',
              { className: 'dsc-row' },
              h(
                'button',
                {
                  type: 'button',
                  className: 'dsc-btn dsc-btn--primary',
                  disabled: busy || flashRunning,
                  onClick: () => request('/flash', { env: flash.env || undefined }),
                },
                t('flashStart'),
              ),
              h(
                'button',
                {
                  type: 'button',
                  className: 'dsc-btn',
                  disabled: busy || !flashRunning,
                  onClick: () => request('/flash/cancel', {}),
                },
                t('flashStop'),
              ),
              h('span', { className: 'dsc-muted dsc-ellipsis' }, `${t('env')} ${flash.env || '-'}`),
            ),
            flash.projectDir
              ? h(
                  'div',
                  { className: 'dsc-muted dsc-mono dsc-ellipsis', title: flash.projectDir },
                  `${t('project')} ${flash.projectDir}`,
                )
              : null,
            flash.error ? h('div', { className: 'dsc-notice' }, flash.error) : null,
          ),
          flash.log && flash.log.length > 0
            ? h('pre', { className: 'dsc-log', ref: scrollRef, tabIndex: -1 }, flash.log.join('\n'))
            : h('div', { className: 'dsc-placeholder' }, t('flashEmpty')),
        )
      }

      return h(
        'div',
        { className: 'dsc-root' },
        h('style', { key: 'css' }, CSS),
        transportError
          ? h('div', { className: 'dsc-notice', key: 'down' }, t('backendDown', { message: transportError }))
          : null,
        statusCard,
        tabsRow,
        pane,
      )
    }

    // ------------------------------------------------------------- plugin

    let t = (key) => key

    function apply(ctx) {
      ctx.effect(() => ctx.locale.register(NS, { zh, en }), 'serial-console: copy')
      t = ctx.locale.bind(NS)

      ctx.effect(
        () =>
          ctx.sidebarRightTabs.register({
            id: TYPE_ID,
            kind: 'serial-console',
            title: () => t('tabTitle'),
            keepMounted: true,
            guide: [
              {
                id: 'serial-console',
                order: 30,
                title: () => t('guideTitle'),
                description: () => t('guideDescription'),
                icon: SerialIcon,
              },
            ],
          }),
        'serial-console: tab type',
      )

      ctx.effect(
        () =>
          ctx.slots.inject('sidebar.right.pane.tab', () =>
            ctx.slots.register(
              {
                name: 'sidebar.right.pane.tab',
                key: TYPE_ID,
                locale: NS,
              },
              SerialConsoleBody,
            ),
          ),
        'serial-console: tab body',
      )

      ctx.effect(
        () =>
          ctx.slots.inject('sidebar.right.pane.tab.title', () =>
            ctx.slots.register(
              {
                name: 'sidebar.right.pane.tab.title',
                key: TYPE_ID,
                locale: NS,
              },
              TabTitle,
            ),
          ),
        'serial-console: tab title',
      )
    }

    return {
      inject: ['slots', 'locale', 'sidebarRightTabs'],
      apply,
    }
  },
})
