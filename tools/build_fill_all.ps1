# Rebuild every "height fill" clip from the original sources.
#
# The device can only crop at runtime after decoding 320x240, which throws away
# resolution. Generating the 4:3 center crop from the original 1280x720 file
# keeps it. Output mirrors the SD card layout:
#   <Stage>\<group>\<title>.avi  ->  /video/.fill/<group>/<title>.avi
#
# The group name comes from the source file name ("<group>-<title>.mp4"), so this
# script stays pure ASCII: Windows PowerShell 5.1 reads .ps1 files as ANSI and
# would garble any non-ASCII literal stored in a UTF-8 file without a BOM.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/build_fill_all.ps1
param(
    [string]$Stage,
    [int]$Fps = 15,
    [int]$Quality = 6,
    [int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Stage) { $Stage = Join-Path $repo 'out-fill' }
# Keep the per-file logs beside the output tree, not inside it: the tree is later
# copied to the card verbatim, and stray .err/.out files must not travel with it.
$logDir = $Stage.TrimEnd('\') + '-logs'
New-Item -ItemType Directory -Force -Path $Stage, $logDir | Out-Null
$log = Join-Path $logDir 'build.log'

function Log([string]$message) {
    $line = ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $message)
    Write-Output $line
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
}

$tasks = New-Object System.Collections.Generic.List[object]
$sources = Get-ChildItem (Join-Path $repo 'raw-video') -Recurse -File -Filter '*.mp4'
foreach ($src in $sources) {
    $dash = $src.BaseName.IndexOf('-')
    if ($dash -lt 1) { continue }
    $group = $src.BaseName.Substring(0, $dash)
    $title = $src.BaseName.Substring($dash + 1)
    $outDir = Join-Path $Stage $group
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $out = Join-Path $outDir ($title + '.avi')
    $hasAudio = (& ffprobe -v error -select_streams a -show_entries stream=codec_type -of csv=p=0 $src.FullName) -ne ''
    $vf = "fps=$Fps,scale=320:240:force_original_aspect_ratio=increase:flags=lanczos,crop=320:240,setsar=1"
    $ffargs = @('-hide_banner', '-loglevel', 'error', '-y', '-i', $src.FullName)
    if ($hasAudio) {
        $ffargs += @('-map', '0:v:0', '-map', '0:a:0')
    } else {
        $ffargs += @('-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo', '-map', '0:v:0', '-map', '1:a:0')
    }
    $ffargs += @('-vf', $vf, '-c:v', 'mjpeg', '-q:v', "$Quality", '-pix_fmt', 'yuvj420p',
                 '-c:a', 'libmp3lame', '-ar', '44100', '-ac', '2', '-b:a', '96k',
                 '-af', 'apad', '-shortest', '-f', 'avi', $out)
    $tasks.Add([pscustomobject]@{ Group = $group; Title = $title; Src = $src.FullName; Out = $out; Args = $ffargs })
}

Log ("start: {0} clips, {1}fps, q{2}, {3} parallel -> {4}" -f $tasks.Count, $Fps, $Quality, $Parallel, $Stage)
$done = 0
$failed = @()
for ($i = 0; $i -lt $tasks.Count; $i += $Parallel) {
    $batch = @()
    $end = [Math]::Min($i + $Parallel, $tasks.Count)
    for ($j = $i; $j -lt $end; ++$j) {
        $task = $tasks[$j]
        $name = $task.Group + '-' + $task.Title
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
        $name = $task.Group + '/' + $task.Title
        $exitCode = $item.Proc.ExitCode
        # Start-Process -PassThru does not always expose ExitCode under 5.1, so the
        # output file plus its probed streams are the authoritative success check.
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
