$ErrorActionPreference = "Continue"

Write-Host "== Java =="
Write-Host "JAVA_HOME=$env:JAVA_HOME"
try { java -version } catch { Write-Host "java not found: $($_.Exception.Message)" }

Write-Host "`n== Gradle =="
$gradle = Get-Command gradle -ErrorAction SilentlyContinue
if ($gradle) {
    Write-Host "gradle=$($gradle.Source)"
    gradle -v
} else {
    Write-Host "gradle not found in PATH"
}

Write-Host "`n== Android SDK =="
Write-Host "ANDROID_HOME=$env:ANDROID_HOME"
Write-Host "ANDROID_SDK_ROOT=$env:ANDROID_SDK_ROOT"
$sdkCandidates = @(
    $env:ANDROID_HOME,
    $env:ANDROID_SDK_ROOT,
    "$env:LOCALAPPDATA\Android\Sdk"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

if (-not $sdkCandidates) {
    Write-Host "Android SDK not found"
} else {
    foreach ($sdk in $sdkCandidates) {
        Write-Host "SDK: $sdk"
        Get-ChildItem "$sdk\ndk" -Directory -ErrorAction SilentlyContinue | Select-Object Name, FullName
        Get-ChildItem "$sdk\cmake" -Directory -ErrorAction SilentlyContinue | Select-Object Name, FullName
    }
}

Write-Host "`n== Suggested build command =="
Write-Host "cd D:\project\_nxemu_src\src\android"
Write-Host "gradle :app:assembleDebug"
