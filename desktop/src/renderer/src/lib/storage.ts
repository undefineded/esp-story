import type { AppSettings, NfcCard } from '@shared/types'
import type { StoryboxResource } from './resources'

const HISTORY_KEY = 'nfc-studio.history.v1'
const SETTINGS_KEY = 'nfc-studio.settings.v1'
const THEME_KEY = 'nfc-studio.theme.v1'
const DEMO_KEY = 'nfc-studio.demo.v1'
const RESOURCES_KEY = 'nfc-studio.resources.v1'

export interface UiSettings {
  autoConnect: boolean
  sound: boolean
  keepHistory: boolean
  pollingInterval: number
}

export const DEFAULT_UI_SETTINGS: UiSettings = {
  autoConnect: true,
  sound: true,
  keepHistory: true,
  pollingInterval: 260
}

function readJson<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key)
    if (!raw) return fallback
    return { ...fallback, ...(JSON.parse(raw) as T) }
  } catch {
    return fallback
  }
}

export function loadUiSettings(): UiSettings {
  return readJson<UiSettings>(SETTINGS_KEY, DEFAULT_UI_SETTINGS)
}

export function saveUiSettings(settings: UiSettings): void {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings))
  } catch {
    /* 忽略存储失败 */
  }
}

export function loadHistory(): NfcCard[] {
  try {
    const raw = localStorage.getItem(HISTORY_KEY)
    if (!raw) return []
    const parsed = JSON.parse(raw) as NfcCard[]
    return Array.isArray(parsed) ? parsed.slice(0, 50) : []
  } catch {
    return []
  }
}

export function hasStoredHistory(): boolean {
  try {
    return localStorage.getItem(HISTORY_KEY) !== null
  } catch {
    return false
  }
}

export function saveHistory(cards: NfcCard[]): void {
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(cards.slice(0, 50)))
  } catch {
    /* 忽略存储失败 */
  }
}

export function loadTheme(): 'light' | 'dark' | 'system' {
  const value = localStorage.getItem(THEME_KEY)
  if (value === 'light' || value === 'dark' || value === 'system') return value
  return 'light'
}

export function saveTheme(theme: 'light' | 'dark' | 'system'): void {
  localStorage.setItem(THEME_KEY, theme)
}

export function isDemoPreferred(): boolean {
  return localStorage.getItem(DEMO_KEY) === '1'
}

export function setDemoPreferred(on: boolean): void {
  if (on) localStorage.setItem(DEMO_KEY, '1')
  else localStorage.removeItem(DEMO_KEY)
}

export interface StoredResources {
  items: StoryboxResource[]
  syncedAt?: number
}

export function loadResources(): StoredResources {
  try {
    const raw = localStorage.getItem(RESOURCES_KEY)
    if (!raw) return { items: [] }
    const parsed = JSON.parse(raw) as Partial<StoredResources>
    const items = Array.isArray(parsed.items)
      ? parsed.items.filter(
          (item): item is StoryboxResource =>
            Boolean(item) &&
            typeof item.id === 'string' &&
            (item.kind === 'music' || item.kind === 'story') &&
            typeof item.path === 'string' &&
            typeof item.title === 'string'
        )
      : []
    return {
      items,
      syncedAt: typeof parsed.syncedAt === 'number' ? parsed.syncedAt : undefined
    }
  } catch {
    return { items: [] }
  }
}

export function saveResources(items: StoryboxResource[], syncedAt: number): void {
  try {
    localStorage.setItem(RESOURCES_KEY, JSON.stringify({ items, syncedAt }))
  } catch {
    /* 忽略存储失败 */
  }
}

export type { AppSettings }
