/* 确保 Electron 二进制存在。
 *
 * 某些网络/代理环境下 electron 包自己的 postinstall 会静默跳过下载，
 * 导致 `npm run dev` 找不到 electron.exe。这里在 npm install 结束时补一次，
 * 并且始终使用国内镜像，避免 GitHub 直连超时。
 */
const { existsSync } = require('node:fs')
const { join } = require('node:path')
const { spawnSync } = require('node:child_process')

const root = join(__dirname, '..')
const electronDir = join(root, 'node_modules', 'electron')
const distDir = join(electronDir, 'dist')

if (!existsSync(distDir)) {
  const installer = join(electronDir, 'install.js')
  if (!existsSync(installer)) {
    console.warn('[ensure-electron] electron/install.js not found; skip')
    process.exit(0)
  }
  console.log('[ensure-electron] Electron binary missing, downloading from npmmirror…')
  const result = spawnSync(process.execPath, [installer], {
    cwd: root,
    stdio: 'inherit',
    env: {
      ...process.env,
      ELECTRON_MIRROR: process.env.ELECTRON_MIRROR || 'https://npmmirror.com/mirrors/electron/'
    }
  })
  if (result.status !== 0 || !existsSync(distDir)) {
    console.warn('[ensure-electron] download failed; set ELECTRON_MIRROR or use a proxy and rerun npm install')
    process.exit(result.status || 1)
  }
}
