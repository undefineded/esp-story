import { app } from 'electron'
import { promises as fs } from 'node:fs'
import { join, dirname } from 'node:path'
import type { AppSettings } from '@shared/types'

const DEFAULTS: AppSettings = {
  autoConnect: true,
  baudRate: 115200
}

/** 连接偏好很小, 直接存 JSON; 主题等纯界面偏好放 localStorage */
export class SettingsStore {
  private file: string
  private data: AppSettings = { ...DEFAULTS }
  private loaded = false

  constructor() {
    this.file = join(app.getPath('userData'), 'settings.json')
  }

  async load(): Promise<AppSettings> {
    if (this.loaded) return this.data
    try {
      const raw = await fs.readFile(this.file, 'utf8')
      const parsed = JSON.parse(raw) as Partial<AppSettings>
      this.data = {
        ...DEFAULTS,
        ...parsed,
        baudRate: parsed.baudRate && parsed.baudRate > 0 ? parsed.baudRate : DEFAULTS.baudRate
      }
    } catch {
      this.data = { ...DEFAULTS }
    }
    this.loaded = true
    return this.data
  }

  get(): AppSettings {
    return { ...this.data }
  }

  async set(patch: Partial<AppSettings>): Promise<AppSettings> {
    this.data = { ...this.data, ...patch }
    if (!this.data.baudRate || this.data.baudRate < 1200) this.data.baudRate = DEFAULTS.baudRate
    try {
      await fs.mkdir(dirname(this.file), { recursive: true })
      await fs.writeFile(this.file, JSON.stringify(this.data, null, 2), 'utf8')
    } catch {
      /* 偏好写入失败不影响使用 */
    }
    return this.get()
  }
}
