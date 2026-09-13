$ErrorActionPreference = 'Stop'
$root = 'D:\NXEmu'
$tools = Join-Path $root 'tools'
$pidFile = Join-Path $tools 'nxemu-diag.pid'
$logFile = Join-Path $tools 'nxemu-diag-host.log'

if (Test-Path -LiteralPath $pidFile) {
    $oldPid = [int](Get-Content -LiteralPath $pidFile -Raw)
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) {
        Write-Output "NXEmu diagnostics already running: PID $oldPid"
        exit 0
    }
}

$process = Start-Process -FilePath 'D:\python\python311\python.exe' `
    -ArgumentList @((Join-Path $tools 'nxemu_diag_server.py'), '--root', $root) `
    -RedirectStandardOutput $logFile -RedirectStandardError "$logFile.err" `
    -WindowStyle Hidden -PassThru
$process.Id | Set-Content -LiteralPath $pidFile -Encoding ascii
Write-Output "NXEmu diagnostics started: PID $($process.Id), http://127.0.0.1:32180"
