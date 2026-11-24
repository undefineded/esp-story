/* 故事机内容库同步: 只从设备 TF 卡的 /music 与 /story 生成可写资源列表。
 *
 * 写入策略是“资源卡片”：不允许自由文本/URL，只把已存在资源编码成一条
 * `storybox://media?path=<SD 绝对路径>` 的 NDEF URI 记录。设备以后解析这条
 * URI 即可定位到具体音乐/故事，不会因为卡片里写的是任意字符串而无法播放。
 */
import type {
  FileBridge,
  FsEntry,
  MediaCatalogInfo,
  MediaStoryPage,
  MediaTrackPage
} from '@shared/types'

export type ResourceKind = 'music' | 'story'

export interface StoryboxResource {
  /** 稳定 id: `${kind}:${path}`，用于界面选择与缓存。 */
  id: string
  kind: ResourceKind
  title: string
  /** 设备目录中的音频路径；故事用音频路径定位。 */
  path: string
  category: string
  /** 音频所在目录；图文故事即故事目录。 */
  directory: string
  size: number
  mtime: number
  illustrated: boolean
  timeline?: string
}

export const RESOURCE_URI_PREFIX = 'storybox://media?path='

const PAGE_SIZE = 64
const MAX_DEPTH = 8
const MAX_ENTRIES = 6000

function baseName(path: string): string {
  const slash = path.lastIndexOf('/')
  return slash < 0 ? path : path.slice(slash + 1)
}

function withoutExtension(name: string): string {
  const dot = name.lastIndexOf('.')
  return dot <= 0 ? name : name.slice(0, dot)
}

function isMp3(entry: FsEntry): boolean {
  return !entry.dir && entry.name.toLowerCase().endsWith('.mp3')
}

function isLrc(entry: FsEntry): boolean {
  return !entry.dir && entry.name.toLowerCase().endsWith('.lrc')
}

function directoryOf(path: string): string {
  const slash = path.lastIndexOf('/')
  return slash <= 0 ? '/' : path.slice(0, slash)
}

async function listAll(files: FileBridge, path: string): Promise<FsEntry[]> {
  const entries: FsEntry[] = []
  let offset = 0
  for (let page = 0; page < 512; ++page) {
    const listing = await files.list(path, offset, PAGE_SIZE)
    entries.push(...listing.entries)
    if (entries.length > MAX_ENTRIES) throw new Error('内容库超过 6000 项，请先在 TF 卡上整理')
    const returned = listing.entries.length
    if (returned === 0) break
    offset += returned
    if (offset >= listing.total) break
  }
  return entries
}

async function safeList(files: FileBridge, path: string): Promise<FsEntry[]> {
  try {
    return await listAll(files, path)
  } catch {
    // 没有 /music、/story 或某个子目录不可读时跳过这一支，其余内容仍可同步。
    return []
  }
}

function addMusic(entry: FsEntry, category: string, out: Map<string, StoryboxResource>): void {
  out.set(`music:${entry.path}`, {
    id: `music:${entry.path}`,
    kind: 'music',
    title: withoutExtension(entry.name),
    path: entry.path,
    category: category || '全部歌曲',
    directory: directoryOf(entry.path),
    size: entry.size,
    mtime: entry.mtime,
    illustrated: false
  })
}

async function walkMusic(
  files: FileBridge,
  directory: string,
  category: string,
  depth: number,
  out: Map<string, StoryboxResource>
): Promise<void> {
  if (depth > MAX_DEPTH) return
  const entries = await safeList(files, directory)
  for (const entry of entries) {
    if (entry.dir) {
      await walkMusic(files, entry.path, category, depth + 1, out)
    } else if (isMp3(entry)) {
      addMusic(entry, category, out)
    }
  }
}

function addStory(
  directory: string,
  audio: FsEntry,
  timeline: FsEntry | undefined,
  category: string,
  out: Map<string, StoryboxResource>
): void {
  const illustrated = Boolean(timeline)
  out.set(`story:${audio.path}`, {
    id: `story:${audio.path}`,
    kind: 'story',
    title: illustrated ? baseName(directory) : withoutExtension(audio.name),
    path: audio.path,
    category: category || '未分类',
    directory,
    size: audio.size,
    mtime: audio.mtime,
    illustrated,
    timeline: timeline?.path
  })
}

async function scanStoryDir(
  files: FileBridge,
  directory: string,
  category: string,
  depth: number,
  out: Map<string, StoryboxResource>
): Promise<void> {
  if (depth > MAX_DEPTH) return
  const entries = await safeList(files, directory)
  const songs = entries.filter(isMp3)
  const timeline = entries.find(isLrc)

  // 与固件故事扫描规则一致: 同目录有音频 + LRC 视为一个图文故事，否则每个
  // MP3 各算一个音频故事；子目录继续扫描并沿用第一层目录名作为分类。
  if (songs.length > 0) {
    if (timeline) addStory(directory, songs[0], timeline, category, out)
    else for (const song of songs) addStory(directory, song, undefined, category, out)
  }

  for (const entry of entries) {
    if (!entry.dir) continue
    await scanStoryDir(files, entry.path, category || entry.name, depth + 1, out)
  }
}

function compare(a: StoryboxResource, b: StoryboxResource): number {
  if (a.kind !== b.kind) return a.kind === 'music' ? -1 : 1
  const category = a.category.localeCompare(b.category, 'zh-CN')
  if (category !== 0) return category
  return a.title.localeCompare(b.title, 'zh-CN')
}

/** 从设备 TF 卡同步内容库；没有 /music 或 /story 时返回空列表。 */
export async function scanResources(files: FileBridge): Promise<StoryboxResource[]> {
  const out = new Map<string, StoryboxResource>()

  for (const entry of await safeList(files, '/music')) {
    if (entry.dir) {
      await walkMusic(files, entry.path, entry.name, 1, out)
    } else if (isMp3(entry)) {
      addMusic(entry, '全部歌曲', out)
    }
  }

  await scanStoryDir(files, '/story', '', 0, out)

  return [...out.values()].sort(compare)
}

/** Source implemented by the USB `media.*` bridge (firmware catalog pages). */
export interface MediaCatalogSource {
  info(): Promise<MediaCatalogInfo>
  tracks(offset?: number, limit?: number): Promise<MediaTrackPage>
  stories(offset?: number, limit?: number): Promise<MediaStoryPage>
}

/** Use the catalog already built by the device instead of re-walking SD. */
export async function scanMediaCatalog(
  media: MediaCatalogSource,
  knownInfo?: MediaCatalogInfo
): Promise<StoryboxResource[]> {
  const info = knownInfo ?? (await media.info())
  const out = new Map<string, StoryboxResource>()

  let offset = 0
  while (offset < info.trackTotal) {
    const page = await media.tracks(offset, 12)
    if (page.tracks.length === 0) break
    for (const [path, name, , category] of page.tracks) {
      if (!path) continue
      const categoryName = category > 0 ? info.categories[category - 1] ?? '未分类' : '全部歌曲'
      out.set(`music:${path}`, {
        id: `music:${path}`,
        kind: 'music',
        title: withoutExtension(name || baseName(path)),
        path,
        category: categoryName,
        directory: directoryOf(path),
        size: 0,
        mtime: 0,
        illustrated: false
      })
    }
    offset += page.tracks.length
    if (offset >= page.total) break
  }

  offset = 0
  while (offset < info.storyTotal) {
    const page = await media.stories(offset, 8)
    if (page.stories.length === 0) break
    for (const [audio, timeline, directory, category] of page.stories) {
      if (!audio) continue
      const illustrated = Boolean(timeline)
      out.set(`story:${audio}`, {
        id: `story:${audio}`,
        kind: 'story',
        title: illustrated ? baseName(directory) : withoutExtension(baseName(audio)),
        path: audio,
        category: category || '未分类',
        directory,
        size: 0,
        mtime: 0,
        illustrated,
        timeline: timeline || undefined
      })
    }
    offset += page.stories.length
    if (offset >= page.total) break
  }

  return [...out.values()].sort(compare)
}

export function buildResourceUri(path: string): string {
  return `${RESOURCE_URI_PREFIX}${path}`
}

export function parseResourceUri(uri: string): string | null {
  if (!uri.startsWith(RESOURCE_URI_PREFIX)) return null
  const path = uri.slice(RESOURCE_URI_PREFIX.length)
  if (!path.startsWith('/music/') && !path.startsWith('/story/')) return null
  return path
}

export function resourceKindFromPath(path: string): ResourceKind | null {
  if (path.startsWith('/music/')) return 'music'
  if (path.startsWith('/story/')) return 'story'
  return null
}

/** Second-level label under a category: folder path relative to the category. */
export function resourceFolderLabel(resource: StoryboxResource): string {
  const root = resource.kind === 'music' ? '/music' : '/story'
  const categoryRoot =
    resource.category === '全部歌曲' || resource.category === '未分类'
      ? root
      : `${root}/${resource.category}`
  const directory = resource.directory || categoryRoot
  if (directory === categoryRoot) return resource.kind === 'music' ? '直属文件' : '本目录'
  if (directory.startsWith(`${categoryRoot}/`)) return directory.slice(categoryRoot.length + 1)
  return directory
}
