. "$PSScriptRoot\env-portable.ps1" | Format-List

Write-Host "`n== Java =="
java -version

Write-Host "`n== Gradle =="
gradle -v

Write-Host "`n== Android SDK installed =="
sdkmanager --sdk_root="$env:ANDROID_HOME" --list_installed

Write-Host "`n== Vulkan shader tools =="
where glslangValidator
glslangValidator --version | Select-Object -First 3

Write-Host "`n== MSVC / MSBuild =="
$vsDevCmd = "D:\VSBuildTools2022\Common7\Tools\VsDevCmd.bat"
if (Test-Path $vsDevCmd) {
    cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && where msbuild && msbuild -version -nologo && where cl && cl /Bv"
} else {
    Write-Host "VS DevCmd not found: $vsDevCmd"
}
