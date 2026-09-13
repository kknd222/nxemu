$pidFile = 'D:\NXEmu\tools\nxemu-diag.pid'
if (Test-Path -LiteralPath $pidFile) {
    $serverPid = [int](Get-Content -LiteralPath $pidFile -Raw)
    Stop-Process -Id $serverPid -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $pidFile -Force
}
