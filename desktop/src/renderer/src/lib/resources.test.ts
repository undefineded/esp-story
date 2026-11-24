import { describe, expect, it } from 'vitest'
import type {
  FileBridge,
  FsEntry,
  MediaCatalogInfo,
  MediaStoryPage,
  MediaTrackPage
} from '@shared/types'
import {
  buildResourceUri,
  parseResourceUri,
  resourceFolderLabel,
  scanMediaCatalog,
  scanResources,
  type MediaCatalogSource,
  type StoryboxResource
} from './resources'

function file(path: string, size = 1024): FsEntry {
  return { name: path.split('/').pop() ?? path, path, dir: false, size, mtime: 1 }
}

function dir(path: string): FsEntry {
  return { name: path.split('/').pop() ?? path, path, dir: true, size: 0, mtime: 1 }
}

function fakeFiles(tree: Record<string, FsEntry[]>, pageSize = 64): FileBridge {
  return {
    list: async (path: string, offset = 0, limit = pageSize) => {
      const all = tree[path] ?? []
      return {
        path,
        parent: '',
        entries: all.slice(offset, offset + limit),
        total: all.length,
        offset,
        limit,
        ready: true,
        capacity: 0,
        used: 0,
        free: 0
      }
    }
  } as unknown as FileBridge
}

function find(resources: StoryboxResource[], path: string): StoryboxResource | undefined {
  return resources.find((item) => item.path === path)
}

describe('scanResources', () => {
  it('maps /music root and nested folders to categories', async () => {
    const files = fakeFiles({
      '/music': [file('/music/A.mp3'), dir('/music/儿歌'), dir('/music/古典')],
      '/music/儿歌': [file('/music/儿歌/B.mp3'), dir('/music/儿歌/深夜')],
      '/music/儿歌/深夜': [file('/music/儿歌/深夜/C.mp3')],
      '/music/古典': [file('/music/古典/D.mp3')],
      '/story': []
    })

    const resources = await scanResources(files)
    expect(resources).toHaveLength(4)
    expect(find(resources, '/music/A.mp3')?.category).toBe('全部歌曲')
    expect(find(resources, '/music/儿歌/B.mp3')?.category).toBe('儿歌')
    expect(find(resources, '/music/儿歌/深夜/C.mp3')?.category).toBe('儿歌')
    expect(find(resources, '/music/古典/D.mp3')?.title).toBe('D')
    expect(resources.every((item) => item.kind === 'music')).toBe(true)
  })

  it('groups story folders into one illustrated story or per-track audio stories', async () => {
    const files = fakeFiles({
      '/music': [],
      '/story': [dir('/story/小熊'), dir('/story/合集'), file('/story/root.mp3')],
      '/story/小熊': [file('/story/小熊/01.mp3'), file('/story/小熊/01.lrc'), file('/story/小熊/cover.jpg')],
      '/story/合集': [file('/story/合集/a.mp3'), file('/story/合集/b.mp3')]
    })

    const resources = await scanResources(files)
    expect(resources).toHaveLength(4)

    const illustrated = find(resources, '/story/小熊/01.mp3')
    expect(illustrated?.illustrated).toBe(true)
    expect(illustrated?.title).toBe('小熊')
    expect(illustrated?.category).toBe('小熊')
    expect(illustrated?.timeline).toBe('/story/小熊/01.lrc')

    const audio = find(resources, '/story/合集/a.mp3')
    expect(audio?.illustrated).toBe(false)
    expect(audio?.title).toBe('a')
    expect(audio?.category).toBe('合集')

    expect(find(resources, '/story/root.mp3')?.category).toBe('未分类')
  })

  it('follows paginated directory listings', async () => {
    const files = fakeFiles(
      {
        '/music': [file('/music/1.mp3'), file('/music/2.mp3'), file('/music/3.mp3')],
        '/story': []
      },
      1
    )

    const resources = await scanResources(files)
    expect(resources.map((item) => item.path).sort()).toEqual([
      '/music/1.mp3',
      '/music/2.mp3',
      '/music/3.mp3'
    ])
  })

  it('returns an empty library when neither root exists', async () => {
    const resources = await scanResources(fakeFiles({}))
    expect(resources).toEqual([])
  })
})

describe('scanMediaCatalog', () => {
  it('maps catalog pages, category names and illustrated stories', async () => {
    const info: MediaCatalogInfo = {
      ready: true,
      trackTotal: 3,
      storyTotal: 1,
      categories: ['儿歌', '小熊的冬天']
    }
    const source: MediaCatalogSource = {
      info: async () => info,
      tracks: async (offset = 0, limit = 24): Promise<MediaTrackPage> => {
        const rows = [
          ['/music/A.mp3', 'A.mp3', 1, 0],
          ['/music/儿歌/B.mp3', 'B.mp3', 2, 1],
          ['/music/儿歌/C.mp3', 'C.mp3', 3, 1]
        ] as MediaTrackPage['tracks']
        return { total: rows.length, offset, limit, tracks: rows.slice(offset, offset + limit) }
      },
      stories: async (offset = 0, limit = 12): Promise<MediaStoryPage> => {
        const rows = [
          ['/story/小熊/01.mp3', '/story/小熊/01.lrc', '/story/小熊', '小熊']
        ] as MediaStoryPage['stories']
        return { total: rows.length, offset, limit, stories: rows.slice(offset, offset + limit) }
      }
    }

    const resources = await scanMediaCatalog(source, info)
    expect(resources).toHaveLength(4)
    expect(find(resources, '/music/A.mp3')?.category).toBe('全部歌曲')
    expect(find(resources, '/music/儿歌/B.mp3')?.category).toBe('儿歌')

    const story = find(resources, '/story/小熊/01.mp3')
    expect(story?.kind).toBe('story')
    expect(story?.illustrated).toBe(true)
    expect(story?.title).toBe('小熊')
    expect(story?.category).toBe('小熊')
  })
})

describe('resourceFolderLabel', () => {
  const base = {
    id: 'music:/music/a.mp3',
    kind: 'music' as const,
    title: 'a',
    path: '/music/a.mp3',
    category: '儿歌',
    directory: '/music/儿歌',
    size: 0,
    mtime: 0,
    illustrated: false
  }

  it('uses the direct category folder and nested subfolders', () => {
    expect(resourceFolderLabel(base)).toBe('直属文件')
    expect(resourceFolderLabel({ ...base, directory: '/music/儿歌/第1季' })).toBe('第1季')
    expect(resourceFolderLabel({ ...base, category: '全部歌曲', directory: '/music' })).toBe('直属文件')
  })

  it('labels story folders and the root bucket', () => {
    const story = {
      ...base,
      kind: 'story' as const,
      category: '图文故事',
      directory: '/story/图文故事/丑小鸭'
    }
    expect(resourceFolderLabel(story)).toBe('丑小鸭')
    expect(resourceFolderLabel({ ...story, category: '未分类', directory: '/story' })).toBe('本目录')
  })
})

describe('resource URI', () => {
  it('round-trips a UTF-8 path through the NDEF URI', () => {
    const path = '/music/小星星 · 童谣合辑.mp3'
    const uri = buildResourceUri(path)
    expect(uri).toBe(`storybox://media?path=${path}`)
    expect(parseResourceUri(uri)).toBe(path)
  })

  it('rejects non-resource URIs', () => {
    expect(parseResourceUri('https://example.com/story')).toBeNull()
    expect(parseResourceUri('storybox://media?path=/video/a.mp4')).toBeNull()
    expect(parseResourceUri('storybox://media?path=')).toBeNull()
  })
})
