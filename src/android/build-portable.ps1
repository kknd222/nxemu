param(
    [string]$Task = ":app:assembleDebug",
    [switch]$WithMsvcEnv
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env-portable.ps1" | Out-Null

Set-Location $PSScriptRoot

if ($WithMsvcEnv) {
    $vsDevCmd = "D:\VSBuildTools2022\Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) {
        throw "VS DevCmd not found: $vsDevCmd"
    }

    $escapedTask = $Task.Replace('"', '\"')
    cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set JAVA_HOME=$env:JAVA_HOME && set ANDROID_HOME=$env:ANDROID_HOME && set ANDROID_SDK_ROOT=$env:ANDROID_SDK_ROOT && set PATH=$env:PATH && gradle $escapedTask --stacktrace"
    exit $LASTEXITCODE
}

gradle $Task --stacktrace
exit $LASTEXITCODE
