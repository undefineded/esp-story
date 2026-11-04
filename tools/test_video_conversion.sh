#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/storybox-video-test.XXXXXX)
ffmpeg -v error -f lavfi -i testsrc2=size=640x360:rate=15 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 1 -c:v mpeg4 -c:a aac "$test_dir/source.mp4"
ffmpeg -v error -f lavfi -i testsrc2=size=320x240:rate=15 -t 1 -c:v mpeg4 "$test_dir/silent.mp4"
node tools/build_video.mjs "$test_dir/source.mp4" "$test_dir/silent.mp4" --out "$test_dir/out" --width 320 --height 240
node tools/build_video.mjs "$test_dir/source.mp4" --out "$test_dir/muted" --no-audio --fps 12 --codec jpeg
node tools/verify_video.mjs "$test_dir/out" "$test_dir/muted"
node --input-type=module - "$test_dir" <<'JS'
import assert from 'node:assert/strict'
import {execFileSync} from 'node:child_process'
import {existsSync} from 'node:fs'
const root = process.argv[2]
for (const file of ['out/source.avi', 'out/silent.avi', 'muted/source.avi']) {
  const path = root + '/' + file
  const info = JSON.parse(execFileSync('ffprobe', ['-v','error','-show_streams','-show_format','-of','json',path]))
  const video = info.streams.find(s => s.codec_type === 'video')
  const audio = info.streams.find(s => s.codec_type === 'audio')
  assert.equal(video.codec_name, 'mjpeg')
  assert.equal(video.width, 320)
  assert.equal(video.height, 240)
  assert.equal(audio.codec_name, 'mp3')
  assert.equal(audio.sample_rate, '44100')
  assert.equal(audio.channels, 2)
  assert(Number(info.format.duration) > 0.9 && Number(info.format.duration) < 1.3)
  assert(!existsSync(path.replace('.avi', '.mp3')))
}
console.log('PASS: MJPEG AVI, embedded MP3, silent input, mute, dimensions and duration')
JS
if node tools/build_video.mjs "$test_dir/source.mp4" --out "$test_dir/rejected" --codec qoi; then exit 1; fi
if node tools/build_video.mjs "$test_dir/source.mp4" --out "$test_dir/rejected" --fps 0; then exit 1; fi
if node tools/build_video.mjs "$test_dir/out/source.avi" --out "$test_dir/out"; then exit 1; fi
if node tools/verify_video.mjs "$test_dir/source.mp4"; then exit 1; fi
if node tools/verify_video.mjs "$test_dir/missing"; then exit 1; fi
