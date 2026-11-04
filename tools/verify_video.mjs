#!/usr/bin/env node
// Asset format validation for the IDF esp_player path (not a hardware performance test).
import { existsSync, readdirSync, statSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { extname, join } from 'node:path'

function collect(target) {
  if (!existsSync(target)) throw new Error('文件不存在: ' + target)
  if (!statSync(target).isDirectory()) return [target]
  return readdirSync(target).filter(n => extname(n).toLowerCase() === '.avi').sort().map(n => join(target, n))
}
function check(file) {
  try {
    const info = JSON.parse(execFileSync('ffprobe', ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', file], {encoding: 'utf8'}))
    const video = info.streams.find(s => s.codec_type === 'video')
    const audio = info.streams.filter(s => s.codec_type === 'audio')
    const issues = []
    if (info.format.format_name !== 'avi' || extname(file).toLowerCase() !== '.avi') issues.push('需要 AVI 容器及 .avi 扩展名')
    if (!video || video.codec_name !== 'mjpeg') issues.push('需要 MJPEG；旧 QOI 请从源视频重新转换')
    if (video && (video.width !== 320 || video.height !== 240)) issues.push('需要 320x240')
    if (audio.some(s => s.codec_name !== 'mp3')) issues.push('内嵌音轨需要 MP3')
    if (issues.length) throw new Error(issues.join('；'))
    console.log('✓ ' + file + '：MJPEG 320x240，' + (audio.length ? '内嵌 MP3' : '无内嵌音轨（外挂 MP3 不会播放）'))
    return true
  } catch (e) {
    console.error('✗ ' + file + ': ' + e.message)
    return false
  }
}
try {
  if (process.argv.length < 3) throw new Error('用法: node tools/verify_video.mjs <文件或目录> [...]')
  const files = process.argv.slice(2).flatMap(collect)
  if (!files.length) throw new Error('没有找到 AVI 文件')
  let failed = 0
  for (const file of files) if (!check(file)) ++failed
  console.log('共 ' + files.length + ' 个视频，' + failed + ' 个需要处理；帧率仍须上板验证')
  if (failed) process.exitCode = 1
} catch (e) {
  console.error(e.message)
  process.exitCode = 1
}
