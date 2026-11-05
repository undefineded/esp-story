# @local/dsh-serial-console

A DSH Web bundle that adds a **串口控制台 / Serial console** tab type to the right sidebar
(`sidebar.right.pane.tab`) and one guide entry as its entry point.

The panel answers three questions in one place:

| Area | What it shows |
|---|---|
| 串口数据 | Live output of the selected COM port, with pause / clear / auto-scroll and a byte + block counter. |
| 连接状态 | The port list (with the Windows device name), the active port and baud rate, connection state, received bytes, and the elapsed connection time. |
| 烧录进度 | A `PlatformIO run -e <env> -t upload` run: percentage parsed from `Writing at … (NN %)`, the current stage, the project directory, and the tail of the upload log. |

## Layout

```
dsh-plugins/serial-console/
├── index.js            Host half: one loopback JSON route + the PowerShell work
├── client.js           Browser half: tab type, guide entry, and the panel view
├── cordis.patch.yml    Bundle patch (row id: serial-console)
├── package.json        dsh.bundle + dsh.client declarations
├── locale/{en,zh}.json Plugin Manager title/description
└── icon.svg            Plugin artwork
```

## Configuration

`cordis.patch.yml` carries the row config; edit it and reload the Host to change these:

| Field | Default | Meaning |
|---|---|---|
| `projectDir` | the path in the patch | Directory holding `platformio.ini`. Empty means the Harness working directory. |
| `env` | `esp32-s3-devkitm-1` | PlatformIO environment used for uploads. |
| `baud` | `115200` | Baud rate a newly opened port starts with. |
| `pioPath` | auto-detect | Explicit `pio.exe`; auto-detect checks `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`, then `PATH`. |

## How the halves talk

The browser half cannot reach Node, and this bundle deliberately does not use the
generated `@Remote`/typert channel, so the Host publishes one loopback-only prefix route:

```
GET  /dsh-serial-console/state?since=<seq>[&ports=1]
POST /dsh-serial-console/{connect,disconnect,clear,flash,flash/cancel}
```

The Client polls `/state` every 700 ms and appends only the chunks newer than its cursor.
Requests are rejected unless `remoteAddress` is a loopback address.

## Agent tool: `serial_console`

The Host half also registers one model-facing tool, so the Agent drives the panel's
single reader instead of opening the COM port itself (which would conflict):

| Action | Returns |
|---|---|
| `status` (default) | connection state, byte/chunk counters, buffer range, ports, flash state, project/env |
| `read` | device output; `since` (a previous `nextSeq`) for only-new data, `limit` 1-500 (newest kept, default 200) |
| `ports` | COM ports with the Windows device name and plug status |
| `flash` | PlatformIO upload status, percentage, stage, and the tail of its log |
| `connect` / `disconnect` | open or close the panel's reader (`port`, `baud`) |

`connect` takes a port number as `"COM5"` **or just `"5"`**, listed or not, and the panel
follows it: the status card switches to that port and the port field shows it. The panel's
port field is also editable (suggestions come from the detected ports), so either side can
set the port for the next connection. Read actions never touch the COM port; `connect` and
`disconnect` change the panel's own state, so the Agent and the panel never own two readers
for one port. Output text is capped at 16 000 characters per call (`truncated` says so).

## Notes

- The serial reader is a PowerShell child process (`[System.IO.Ports.SerialPort]`) that
  streams raw bytes framed as base64, so the Host needs no native module. DTR/RTS stay low
  so opening a port does not reset the board.
- Starting a flash disconnects the serial reader first: the port is exclusive.
- Cancelling kills the whole process tree (`taskkill /T`) so `pio`'s Python child releases the port.
- The route is unauthenticated by design on loopback only; do not expose it by binding the
  Web server to a non-loopback address.
