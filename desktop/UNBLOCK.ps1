# 移除 SmartScreen 的“下载标记”（不修改文件内容）
# 用法：右键“使用 PowerShell 运行”，或在 PowerShell 中执行:
#   pwsh -ExecutionPolicy Bypass -File .\UNBLOCK.ps1
$root = Split-Path -Parent $PSScriptRoot
$targets = @(
  (Join-Path $root 'dist\win-unpacked'),
  (Join-Path $root 'dist')
)
$count = 0
foreach ($dir in $targets) {
  if (-not (Test-Path $dir)) { continue }
  Get-ChildItem -Path $dir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.exe', '.dll' } |
    ForEach-Object {
      try {
        Unblock-File -LiteralPath $_.FullName -ErrorAction Stop
        $count++
      } catch {
        Write-Warning "跳过 $($_.FullName): $($_.Exception.Message)"
      }
    }
}
Write-Host "已处理 $count 个文件。"
Write-Host "如果仍出现 SmartScreen，请点击“更多信息 -> 仍要运行”，或直接双击 run-demo.cmd / run.cmd。"
