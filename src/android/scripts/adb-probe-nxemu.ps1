param(
    [string]$Adb = 'D:\project\qwenasr\mobile\android-webview\.tools\android-sdk\platform-tools\adb.exe',
    [string]$Apk = 'D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk',
    [string]$GamePath = '/sdcard/ns/rom/Metal Dogs [0100A6E01681C000][v0][JP].dnsp',
    [int]$InitialWaitSec = 5,
    [int]$AfterInputWaitSec = 8,
    [switch]$Install,
    [int]$Nce = 1,
    [int]$NceTrace = 0,
    [int]$NceAltStack = -1,
    [int]$PerfHudDetailed = -1,
    [int]$GraphicsCompat = -1,
    [int]$FrameSkip = -1,
    [int]$ResolutionSetup = -1,
    [int]$AspectRatio = -1,
    [int]$AstcMode = -1,
    [int]$GpuAccuracy = -1,
    [int]$ReactiveFlushing = -1,
    [int]$PipelineCache = -1,
    [int]$AsyncShaders = -1,
    [int]$ForceAstcTranscode = -1,
    [int]$ForceBcnTranscode = -1,
    [int]$SquashedIteratedBlend = -1,
    [int]$RenderDiag = -1,
    [int]$DisableDynamicState3Blending = -1,
    [int]$ForceOpaqueLayers = -1,
    [int]$DisableAcceleratedDisplay = -1,
    [int]$ForceDrawColorClear = -1,
    [switch]$ClearGraphicsCaches,
    [int]$SendA = 1,
    [int]$SendARepeats = 1,
    [int]$SendAIntervalSec = 8,
    [string]$InputSequence = '',
    [string]$OutRoot = 'D:\project\_nxemu_src\phone-logs',
    [switch]$Adaptive,
    [int]$MaxWaitSec = 0,
    [int]$PollSec = 2,
    [string]$StopOnPattern = 'FATAL EXCEPTION|SIGSEGV|signal 11|signal 6|Render\.Vulkan <Error>|LoadRom failed|renderStall=detected|ANR in org\.nxemu',
    [int]$ScreenshotIntervalSec = 0,
    [int]$ScreenshotMaxCount = 8,
    [switch]$NoFinalStop
)

$ErrorActionPreference = 'Continue'
$pkg = 'org.nxemu.app.debug'
if (!(Test-Path -LiteralPath $Adb)) { throw "adb not found: $Adb" }

$out = Join-Path $OutRoot ('probe-script-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force -Path $out | Out-Null
$script:ProbeOutDir = $out
$script:ScreenshotCount = 0
$script:LastScreenshotTime = Get-Date '2000-01-01T00:00:00'
$script:RemoteSessionMarker = '/sdcard/ns/logs/nxemu-probe-start.marker'

function Invoke-AdbShell([string]$Command) {
    & $Adb shell $Command
}

function Unlock-Phone {
    $policy = (& $Adb shell dumpsys window policy) -join "`n"
    if ($policy -match 'screenState=SCREEN_STATE_OFF' -or $policy -match 'mInputRestricted=true' -or $policy -match 'showing=true') {
        & $Adb shell input keyevent 224 | Out-Null
        Start-Sleep -Milliseconds 500
        & $Adb shell input swipe 620 2100 620 800 300 | Out-Null
        Start-Sleep -Milliseconds 500
        & $Adb shell input text 570765 | Out-Null
        & $Adb shell input keyevent 66 | Out-Null
        & $Adb shell wm dismiss-keyguard | Out-Null
        Start-Sleep -Seconds 1
    }
}

function Pull-LatestSession([string]$OutDir) {
    # Session filenames include yyyyMMdd-HHmmss; after filtering by a per-probe marker,
    # lexical sort is enough and avoids brittle Android shell quoting around sort/cut/awk.
    $filtered = (& $Adb shell "find /sdcard/ns/logs -name 'nxemu-session-*.txt' -newer $script:RemoteSessionMarker 2>/dev/null | sort | tail -n 1")
    $latest = ($filtered | Where-Object { $_ -match '^/sdcard/' } | Select-Object -First 1)
    $latest = if ($latest) { $latest.Trim() } else { '' }
    if (-not $latest) {
        $latest = (& $Adb shell 'ls -t /sdcard/ns/logs/nxemu-session-*.txt 2>/dev/null | head -n 1')
        $latest = ($latest | Where-Object { $_ -match '^/sdcard/' } | Select-Object -First 1)
        $latest = if ($latest) { $latest.Trim() } else { '' }
        if ($latest) {
            "sessionSource=fallback-latest $latest" | Set-Content -LiteralPath (Join-Path $OutDir 'session-source.txt')
        }
    } else {
        "sessionSource=marker-filtered $latest" | Set-Content -LiteralPath (Join-Path $OutDir 'session-source.txt')
    }
    if ($latest) { & $Adb pull $latest (Join-Path $OutDir 'session.txt') | Out-Null }
}

function Save-ProbeScreenshot([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($script:ProbeOutDir)) { return }
    $safeName = ($Name -replace '[^A-Za-z0-9._-]', '-')
    if ([string]::IsNullOrWhiteSpace($safeName)) { $safeName = 'screen' }
    & $Adb shell screencap -p /sdcard/ns/logs/nxemu-last-screen.png | Out-Null
    & $Adb pull /sdcard/ns/logs/nxemu-last-screen.png (Join-Path $script:ProbeOutDir $safeName) | Out-Null
}

function Maybe-CaptureIntervalScreenshot([int]$ElapsedSeconds) {
    if ($ScreenshotIntervalSec -le 0) { return }
    if ($ScreenshotMaxCount -le 0) { return }
    if ($script:ScreenshotCount -ge $ScreenshotMaxCount) { return }

    $now = Get-Date
    if ((($now - $script:LastScreenshotTime).TotalSeconds) -lt $ScreenshotIntervalSec) { return }

    $script:ScreenshotCount++
    $script:LastScreenshotTime = $now
    $name = ('screen-{0:D3}-t{1:D4}s.png' -f $script:ScreenshotCount, [Math]::Max(0, $ElapsedSeconds))
    Save-ProbeScreenshot $name
}

function Get-AppPid {
    $pidText = (& $Adb shell "pidof $pkg 2>/dev/null" | Select-Object -First 1)
    if ($pidText) { return $pidText.Trim() }
    return ''
}

function Test-AppAlive {
    return -not [string]::IsNullOrWhiteSpace((Get-AppPid))
}

function Get-RecentInterestingLog([string]$Pattern, [int]$Tail = 1200) {
    if ([string]::IsNullOrWhiteSpace($Pattern)) { return '' }
    $recent = (& $Adb logcat -d -t $Tail -v threadtime) -join "`n"
    $match = [regex]::Match($recent, $Pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($match.Success) { return $match.Value }
    return ''
}

function Wait-AdaptiveProbe([int]$MaxSeconds, [int]$PollSeconds, [string]$Pattern) {
    if ($MaxSeconds -le 0) { return 'adaptive skipped: MaxWaitSec<=0' }
    $poll = [Math]::Max(1, $PollSeconds)
    $start = Get-Date
    while ($true) {
        $elapsed = [int]((Get-Date) - $start).TotalSeconds
        Maybe-CaptureIntervalScreenshot $elapsed
        if ($elapsed -ge $MaxSeconds) {
            return "max-wait reached ${elapsed}s"
        }

        if (-not (Test-AppAlive)) {
            return "app exited at ${elapsed}s"
        }

        $hit = Get-RecentInterestingLog $Pattern 1600
        if (![string]::IsNullOrWhiteSpace($hit)) {
            return "matched pattern '$hit' at ${elapsed}s"
        }

        Start-Sleep -Seconds ([Math]::Min($poll, [Math]::Max(1, $MaxSeconds - $elapsed)))
    }
}

function Send-DebugButton([string]$Button, [int]$HoldMs = 160) {
    if ([string]::IsNullOrWhiteSpace($Button)) { return }
    & $Adb shell am broadcast -a org.nxemu.app.DEBUG_INPUT --es button $Button --el holdMs $HoldMs | Out-Null
}

function Clear-AppGraphicsCaches {
    # Keep this intentionally narrow: remove nxemu/yuzu shader/pipeline/driver
    # caches only, not saves/config/ROMs. This mirrors GpuDriverHelper.clearGraphicsCaches()
    # and also covers files/user/shader where Vulkan pipeline caches are actually written.
    & $Adb shell "run-as $pkg sh -c 'rm -rf cache/* files/user/shader files/user/pipeline_cache files/user/vulkan_cache files/user/cache files/shader files/shader_caches files/pipeline_cache files/vulkan_cache files/gpu/cache 2>/dev/null; mkdir -p files/user/shader files/user/cache'" | Out-Null
    & $Adb shell "rm -rf /sdcard/Android/data/$pkg/cache/* /sdcard/Android/data/$pkg/files/gpu/vk_file_redirect /sdcard/Android/data/$pkg/files/gpu/shader_cache /sdcard/Android/data/$pkg/files/gpu/pipeline_cache 2>/dev/null; mkdir -p /sdcard/Android/data/$pkg/files/gpu/vk_file_redirect" | Out-Null
}

function Invoke-InputSequence([string]$Sequence) {
    if ([string]::IsNullOrWhiteSpace($Sequence)) { return }
    # Format:
    #   "A@5,DOWN@2,DOWN@1,A@1"       means sleep N seconds before each 160ms button.
    #   "A@5:800,DOWN@2:500,A@1:800"  additionally sets hold time in ms.
    foreach ($token in ($Sequence -split ',')) {
        $part = $token.Trim()
        if (!$part) { continue }
        $button = $part
        $delay = 0
        $hold = 160
        if ($part -match '^([^@:]+)@(\d+):(\d+)$') {
            $button = $Matches[1].Trim()
            $delay = [int]$Matches[2]
            $hold = [int]$Matches[3]
        } elseif ($part -match '^([^@:]+)@(\d+)$') {
            $button = $Matches[1].Trim()
            $delay = [int]$Matches[2]
        } elseif ($part -match '^([^@:]+):(\d+)$') {
            $button = $Matches[1].Trim()
            $hold = [int]$Matches[2]
        }
        if ($delay -gt 0) { Start-Sleep -Seconds $delay }
        Send-DebugButton $button $hold
    }
}

Unlock-Phone
if ($Install) { & $Adb install -r -d $Apk }
& $Adb shell logcat -c | Out-Null
& $Adb shell setprop debug.nxemu.nce ($(if($Nce){'1'}else{'0'})) | Out-Null
& $Adb shell setprop debug.nxemu.nce.trace ($(if($NceTrace){'1'}else{'0'})) | Out-Null
if ($NceAltStack -ge 0) {
    & $Adb shell setprop debug.nxemu.nce.altstack ($(if($NceAltStack -ne 0){'true'}else{'false'})) | Out-Null
}
if ($AstcMode -ge 0) {
    & $Adb shell setprop debug.nxemu.astc_mode ([Math]::Min(2, [Math]::Max(0, $AstcMode))) | Out-Null
}
if ($GpuAccuracy -ge 0) {
    & $Adb shell setprop debug.nxemu.gpu_accuracy ([Math]::Min(2, [Math]::Max(0, $GpuAccuracy))) | Out-Null
}
if ($ReactiveFlushing -ge 0) {
    & $Adb shell setprop debug.nxemu.reactive_flushing ($(if($ReactiveFlushing -ne 0){'true'}else{'false'})) | Out-Null
}
if ($PipelineCache -ge 0) {
    & $Adb shell setprop debug.nxemu.pipeline_cache ($(if($PipelineCache -ne 0){'true'}else{'false'})) | Out-Null
}
if ($AsyncShaders -ge 0) {
    & $Adb shell setprop debug.nxemu.force_async_shaders ($(if($AsyncShaders -ne 0){'true'}else{'false'})) | Out-Null
}
if ($ForceAstcTranscode -ge 0) {
    & $Adb shell setprop debug.nxemu.force_astc_transcode ($(if($ForceAstcTranscode -ne 0){'true'}else{'false'})) | Out-Null
}
if ($ForceBcnTranscode -ge 0) {
    & $Adb shell setprop debug.nxemu.force_bcn_transcode ($(if($ForceBcnTranscode -ne 0){'true'}else{'false'})) | Out-Null
}
if ($SquashedIteratedBlend -ge 0) {
    & $Adb shell setprop debug.nxemu.squashed_iterated_blend ($(if($SquashedIteratedBlend -ne 0){'true'}else{'false'})) | Out-Null
}
if ($RenderDiag -ge 0) {
    & $Adb shell setprop debug.nxemu.render_diag ($(if($RenderDiag -ne 0){'true'}else{'false'})) | Out-Null
}
if ($DisableDynamicState3Blending -ge 0) {
    & $Adb shell setprop debug.nxemu.disable_dynamic_state3_blending ($(if($DisableDynamicState3Blending -ne 0){'true'}else{'false'})) | Out-Null
}
if ($ForceOpaqueLayers -ge 0) {
    & $Adb shell setprop debug.nxemu.force_opaque_layers ($(if($ForceOpaqueLayers -ne 0){'true'}else{'false'})) | Out-Null
}
if ($DisableAcceleratedDisplay -ge 0) {
    & $Adb shell setprop debug.nxemu.disable_accelerated_display ($(if($DisableAcceleratedDisplay -ne 0){'true'}else{'false'})) | Out-Null
}
if ($ForceDrawColorClear -ge 0) {
    & $Adb shell setprop debug.nxemu.force_draw_color_clear ($(if($ForceDrawColorClear -ne 0){'true'}else{'false'})) | Out-Null
}
& $Adb shell am force-stop $pkg | Out-Null
if ($ClearGraphicsCaches) { Clear-AppGraphicsCaches }
& $Adb shell "mkdir -p /sdcard/ns/logs; rm -f $script:RemoteSessionMarker; touch $script:RemoteSessionMarker" | Out-Null
Start-Sleep -Milliseconds 500

# Important: run through a single remote shell command so the ROM path with spaces stays quoted.
$nceText = if ($Nce -ne 0) { 'true' } else { 'false' }
$hudExtra = ''
if ($PerfHudDetailed -ge 0) {
    $hudText = if ($PerfHudDetailed -ne 0) { 'true' } else { 'false' }
    $hudExtra = " --ez org.nxemu.app.EXTRA_PERF_HUD_DETAILED $hudText"
}
$compatExtra = ''
if ($GraphicsCompat -ge 0) {
    $compatText = if ($GraphicsCompat -ne 0) { 'true' } else { 'false' }
    $compatExtra = " --ez org.nxemu.app.EXTRA_GRAPHICS_COMPAT $compatText"
}
$frameExtra = if ($FrameSkip -ge 0) { " --ei org.nxemu.app.EXTRA_FRAME_SKIP $([Math]::Min(4, [Math]::Max(0, $FrameSkip)))" } else { '' }
$resExtra = if ($ResolutionSetup -ge 0) { " --ei org.nxemu.app.EXTRA_RESOLUTION_SETUP $([Math]::Min(2, [Math]::Max(0, $ResolutionSetup)))" } else { '' }
$aspectExtra = if ($AspectRatio -ge 0) { " --ei org.nxemu.app.EXTRA_ASPECT_RATIO $([Math]::Min(4, [Math]::Max(0, $AspectRatio)))" } else { '' }
$remote = "am start -n $pkg/org.nxemu.app.EmulationActivity --es org.nxemu.app.EXTRA_GAME_PATH '$GamePath' --ez org.nxemu.app.EXTRA_AUTO_OUTPUT_LOG true --ez org.nxemu.app.EXTRA_PREFER_NCE $nceText$hudExtra$compatExtra$frameExtra$resExtra$aspectExtra"
Invoke-AdbShell $remote | Out-Host

Start-Sleep -Seconds $InitialWaitSec
$stopReason = ''
if ($Adaptive -and -not (Test-AppAlive)) {
    $stopReason = 'app exited during initial wait'
}

if (![string]::IsNullOrWhiteSpace($InputSequence)) {
    if ([string]::IsNullOrWhiteSpace($stopReason)) {
        Invoke-InputSequence $InputSequence
        if ($Adaptive) {
            $wait = if ($MaxWaitSec -gt 0) { $MaxWaitSec } else { $AfterInputWaitSec }
            $stopReason = Wait-AdaptiveProbe $wait $PollSec $StopOnPattern
        } elseif ($AfterInputWaitSec -gt 0) {
            Start-Sleep -Seconds $AfterInputWaitSec
        }
    }
} elseif ($SendA -ne 0) {
    if ([string]::IsNullOrWhiteSpace($stopReason)) {
        $repeats = [Math]::Max(1, $SendARepeats)
        $remaining = [Math]::Max(0, $AfterInputWaitSec)
        for ($i = 0; $i -lt $repeats; $i++) {
            Send-DebugButton A 160
            if ($Adaptive -and -not (Test-AppAlive)) {
                $stopReason = "app exited after A repeat $($i + 1)"
                break
            }
            if ($i -lt ($repeats - 1)) {
                $sleep = [Math]::Min([Math]::Max(1, $SendAIntervalSec), $remaining)
                if ($sleep -gt 0) {
                    Start-Sleep -Seconds $sleep
                    $remaining -= $sleep
                }
            }
        }
        if ([string]::IsNullOrWhiteSpace($stopReason)) {
            if ($Adaptive) {
                $wait = if ($MaxWaitSec -gt 0) { $MaxWaitSec } else { $remaining }
                $stopReason = Wait-AdaptiveProbe $wait $PollSec $StopOnPattern
            } elseif ($remaining -gt 0) {
                Start-Sleep -Seconds $remaining
            }
        }
    }
} elseif ($Adaptive) {
    $wait = if ($MaxWaitSec -gt 0) { $MaxWaitSec } else { $AfterInputWaitSec }
    $stopReason = Wait-AdaptiveProbe $wait $PollSec $StopOnPattern
}

Save-ProbeScreenshot 'screen.png'
& $Adb logcat -d -t 80000 -v threadtime > (Join-Path $out 'logcat-tail.txt')
Pull-LatestSession $out

$alive = Test-AppAlive
$summary = @(
    "time=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff zzz')",
    "package=$pkg",
    "adaptive=$($Adaptive.IsPresent)",
    "stopReason=$stopReason",
    "appAlive=$alive",
    "pid=$(Get-AppPid)",
    "initialWaitSec=$InitialWaitSec",
    "afterInputWaitSec=$AfterInputWaitSec",
    "nce=$Nce",
    "nceTrace=$NceTrace",
    "nceAltStack=$NceAltStack",
    "perfHudDetailed=$PerfHudDetailed",
    "graphicsCompat=$GraphicsCompat",
    "frameSkip=$FrameSkip",
    "resolutionSetup=$ResolutionSetup",
    "aspectRatio=$AspectRatio",
    "astcMode=$AstcMode",
    "gpuAccuracy=$GpuAccuracy",
    "reactiveFlushing=$ReactiveFlushing",
    "pipelineCache=$PipelineCache",
    "asyncShaders=$AsyncShaders",
    "forceAstcTranscode=$ForceAstcTranscode",
    "forceBcnTranscode=$ForceBcnTranscode",
    "squashedIteratedBlend=$SquashedIteratedBlend",
    "renderDiag=$RenderDiag",
    "disableDynamicState3Blending=$DisableDynamicState3Blending",
    "forceOpaqueLayers=$ForceOpaqueLayers",
    "disableAcceleratedDisplay=$DisableAcceleratedDisplay",
    "forceDrawColorClear=$ForceDrawColorClear",
    "clearGraphicsCaches=$($ClearGraphicsCaches.IsPresent)",
    "maxWaitSec=$MaxWaitSec",
    "pollSec=$PollSec",
    "screenshotIntervalSec=$ScreenshotIntervalSec",
    "screenshotMaxCount=$ScreenshotMaxCount",
    "screenshotCount=$script:ScreenshotCount",
    "stopOnPattern=$StopOnPattern",
    "inputSequence=$InputSequence"
)
Set-Content -LiteralPath (Join-Path $out 'probe-summary.txt') -Value $summary -Encoding UTF8

Write-Host "OUT=$out"
Write-Host "STOP_REASON=$stopReason"
Write-Host "APP_ALIVE=$alive"
if (Test-Path (Join-Path $out 'session.txt')) {
    Select-String -LiteralPath (Join-Path $out 'session.txt') -Pattern 'perfOverlay|nceEnabled|cpuBackendActual|vulkanPresentCount|vulkanCompositeCount|svcEventCount|SVC.Call|SVC.Return|OS.Thread|CPU.Core|renderStall|currentStage' | Select-Object -Last 120
}

if (-not $NoFinalStop) {
    & $Adb shell am force-stop $pkg | Out-Null
    & $Adb shell input keyevent 26 | Out-Null
}


