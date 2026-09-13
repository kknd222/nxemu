$ErrorActionPreference = "Stop"

$QwenMobileTools = "D:\project\qwenasr\mobile\android-webview\.tools"
$VsBuildTools = "D:\VSBuildTools2022"
$VulkanSdk = "C:\VulkanSDK\1.4.357.0"

$env:JAVA_HOME = Join-Path $QwenMobileTools "jdk17\jdk-17.0.18+8"
$env:ANDROID_HOME = Join-Path $QwenMobileTools "android-sdk"
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$env:GRADLE_HOME = Join-Path $QwenMobileTools "gradle\gradle-8.11.1"

$pathEntries = @(
    (Join-Path $env:JAVA_HOME "bin"),
    (Join-Path $env:GRADLE_HOME "bin"),
    (Join-Path $env:ANDROID_HOME "cmdline-tools\latest\bin"),
    (Join-Path $env:ANDROID_HOME "platform-tools"),
    (Join-Path $env:ANDROID_HOME "cmake\3.31.6\bin")
)

if (Test-Path (Join-Path $VulkanSdk "Bin\glslangValidator.exe")) {
    $pathEntries += (Join-Path $VulkanSdk "Bin")
    $env:VULKAN_SDK = $VulkanSdk
}

$env:PATH = (($pathEntries + @($env:PATH)) -join ";")

[pscustomobject]@{
    JAVA_HOME = $env:JAVA_HOME
    ANDROID_HOME = $env:ANDROID_HOME
    GRADLE_HOME = $env:GRADLE_HOME
    VULKAN_SDK = $env:VULKAN_SDK
    VS_DEV_CMD = Join-Path $VsBuildTools "Common7\Tools\VsDevCmd.bat"
}
