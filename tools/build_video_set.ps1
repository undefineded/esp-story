# Build the card assets for a batch of series folders: the letterboxed 320x240
# AVI (the file the library lists) plus its full-height mirror under .fill/.
#
# The group name becomes the first directory level under /video, which is what
# the video list shows as a folder. Titles come from the file name with the series
# prefix/suffix stripped where it would only repeat the folder name.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/build_video_set.ps1
#        also: -Fps 15 -Quality 6 -Parallel 6 -Stage out-video-new
param(
    [string]$Stage,
    [int]$Fps = 15,
    [int]$Quality = 6,
    [int]$Parallel = 6
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Stage) { $Stage = Join-Path $repo 'out-video-new' }
$logDir = $Stage.TrimEnd('\') + '-logs'
New-Item -ItemType Directory -Force -Path $Stage, $logDir | Out-Null
$log = Join-Path $logDir 'build.log'

function Log([string]$message) {
    $line = ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $message)
    Write-Output $line
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
}

# Folder under raw-video -> group folder on the card.
$sets = @(
    @{ Folder = '02-宝宝巴士启蒙音乐剧之动物世界-18集'; Group = '动物世界'; Strip = '^' },
    @{ Folder = '06-宝宝巴士启蒙音乐剧之好习惯养成-33集'; Group = '好习惯养成'; Strip = '^' },
    @{ Folder = '宝宝巴士之好情绪好品格 全30集'; Group = '好情绪好品格'; Strip = '^好情绪好品格\s*' },
    @{ Folder = '贝瓦学堂'; Group = '贝瓦学堂'; Strip = '^贝瓦儿歌\s*' }
)

$tasks = New-Object System.Collections.Generic.List[object]
foreach ($set in $sets) {
    $dir = Join-Path (Join-Path $repo 'raw-video') $set.Folder
    if (-not (Test-Path $dir)) { Log ("missing source dir: {0}" -f $dir); continue }
    $normalDir = Join-Path $Stage $set.Group
    $fillDir = Join-Path (Join-Path $Stage '.fill') $set.Group
    New-Item -ItemType Directory -Force -Path $normalDir, $fillDir | Out-Null
    foreach ($src in Get-ChildItem $dir -File -Filter '*.mp4') {
        $title = $src.BaseName -replace $set.Strip, ''
        $title = $title -replace '\s*-超清720P\s*\.?\s*$', ''
        $title = $title.Trim()
        if (-not $title) { $title = $src.BaseName.Trim() }
        $hasAudio = (& ffprobe -v error -select_streams a -show_entries stream=codec_type -of csv=p=0 $src.FullName) -ne ''
        foreach ($variant in 'normal', 'fill') {
            if ($variant -eq 'normal') {
                $out = Join-Path $normalDir ($title + '.avi')
                $vf = "fps=$Fps,scale=320:240:force_original_aspect_ratio=decrease:flags=lanczos,pad=320:240:(320-iw)/2:(240-ih)/2:color=black,setsar=1"
            } else {
                $out = Join-Path $fillDir ($title + '.avi')
                $vf = "fps=$Fps,scale=320:240:force_original_aspect_ratio=increase:flags=lanczos,crop=320:240,setsar=1"
            }
            $ffargs = @('-hide_banner', '-loglevel', 'error', '-y', '-i', $src.FullName)
            if ($hasAudio) {
                $ffargs += @('-map', '0:v:0', '-map', '0:a:0')
            } else {
                $ffargs += @('-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo', '-map', '0:v:0', '-map', '1:a:0')
            }
            $ffargs += @('-vf', $vf, '-c:v', 'mjpeg', '-q:v', "$Quality", '-pix_fmt', 'yuvj420p',
                         '-c:a', 'libmp3lame', '-ar', '44100', '-ac', '2', '-b:a', '96k',
                         '-af', 'apad', '-shortest', '-f', 'avi', $out)
            $tasks.Add([pscustomobject]@{ Group = $set.Group; Title = $title; Variant = $variant; Src = $src.FullName; Out = $out; Args = $ffargs })
        }
    }
    Log ("queued {0}: {1} sources" -f $set.Group, (Get-ChildItem $dir -File -Filter '*.mp4').Count)
}

Log ("start: {0} encodes ({1}fps, q{2}, {3} parallel) -> {4}" -f $tasks.Count, $Fps, $Quality, $Parallel, $Stage)
$done = 0
$failed = @()
for ($i = 0; $i -lt $tasks.Count; $i += $Parallel) {
    $batch = @()
    $end = [Math]::Min($i + $Parallel, $tasks.Count)
    for ($j = $i; $j -lt $end; ++$j) {
        $task = $tasks[$j]
        $name = $task.Group + '-' + $task.Title + '-' + $task.Variant
        $errFile = Join-Path $logDir ($name + '.err')
        $outFile = Join-Path $logDir ($name + '.out')
        $argLine = ($task.Args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
        $proc = Start-Process -FilePath 'ffmpeg' -ArgumentList $argLine -NoNewWindow -PassThru `
                              -RedirectStandardError $errFile -RedirectStandardOutput $outFile
        $batch += [pscustomobject]@{ Task = $task; Proc = $proc }
    }
    foreach ($item in $batch) {
        $item.Proc.WaitForExit()
        $task = $item.Task
        $name = $task.Group + '/' + $task.Title + ' (' + $task.Variant + ')'
        $exitCode = $item.Proc.ExitCode
        if (-not (Test-Path $task.Out) -or ($null -ne $exitCode -and $exitCode -ne 0)) {
            $failed += $name
            Log ("FAIL {0} exit={1}" -f $name, $exitCode)
            continue
        }
        $dims = (& ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 $task.Out).Trim()
        $audio = (& ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 $task.Out).Trim()
        if ($dims -ne '320,240' -or $audio -ne 'mp3') {
            $failed += $name
            Log ("FAIL {0} dims={1} audio={2}" -f $name, $dims, $audio)
            continue
        }
        $seconds = [double](& ffprobe -v error -show_entries format=duration -of csv=p=0 $task.Out)
        $kbps = if ($seconds -gt 0) { [math]::Round((Get-Item $task.Out).Length / 1024 / $seconds) } else { 0 }
        ++$done
        Log ("OK {0} {1} KB/s" -f $name, $kbps)
    }
}
Log ("done: {0}/{1} ok, {2} failed" -f $done, $tasks.Count, $failed.Count)
foreach ($name in $failed) { Log ("FAILED {0}" -f $name) }
