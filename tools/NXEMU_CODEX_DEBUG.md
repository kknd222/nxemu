# NXEmu automated development and diagnostics

## Installed locations

- Runtime: `D:\NXEmu`
- Source: `D:\project\_nxemu_official_20260913`
- ROM: `F:\NS\[0100F08026D0C000] [v0].dxci`
- Runtime log: `D:\NXEmu\user\log\nxemu_log.txt`
- Structured timeline: `D:\NXEmu\user\log\nxemu_events.jsonl`
- Diagnostics API: `http://127.0.0.1:32180`

## Diagnostics service

Start and stop the independent service with:

```powershell
& D:\NXEmu\tools\start-nxemu-diag.ps1
& D:\NXEmu\tools\stop-nxemu-diag.ps1
```

Read-only endpoints:

```text
GET /health
GET /status
GET /events?since=0
GET /log/tail?lines=200
```

Lifecycle endpoints (localhost only):

```text
POST /control/start  body: {} or {"rom":"F:\\path\\game.dxci"}
POST /control/stop   body: {} or {"force":true}
```

The normal stop endpoint posts `WM_CLOSE`, allowing NXEmu to show its confirmation dialog. Force stop is only for a hung process.

## Build and deploy

Build all Windows components:

```powershell
& 'D:\VSBuildTools2022\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  D:\project\_nxemu_official_20260913\nxemu.sln /m /t:nxemu `
  /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Do not deploy loaded DLLs. Stop NXEmu first, back up the existing files, then copy:

```text
bin\x64\Release\nxemu.exe                         -> D:\NXEmu\nxemu.exe
modules\x64\loader\nxemu-loader.dll              -> D:\NXEmu\modules\loader\nxemu-loader.dll
modules\x64\operating_system\nxemu-os.dll        -> D:\NXEmu\modules\operating_system\nxemu-os.dll
modules\x64\cpu\nxemu-cpu.dll                    -> D:\NXEmu\modules\cpu\nxemu-cpu.dll
modules\x64\video\nxemu-video.dll                -> D:\NXEmu\modules\video\nxemu-video.dll
```

Verify source and destination SHA-256 hashes after every deployment.

## Test milestones

Expected event order:

```text
rom_identified -> program_loaded -> romfs_ready -> video_started
-> keyboard_opened -> keyboard_submitted -> save_requested
-> save_ready -> save_filesystem_opened -> save_io -> save_committed
```

Absence of a later event localizes the failing subsystem. Scene-specific game progress requires optional framebuffer sampling/OCR; capture only the NXEmu render target or window.

## Upstream synchronization

Remote `kknd222` has `master`, `android-nxemu-port-20260901`, and `fix-mk8-patch-storage`.
The long-lived private integration branch contains the full history of both feature branches. Sync `master` in a temporary branch, resolve equivalent functionality in favor of upstream, preserve private-only patches as small commits, then run Windows and Android regression tests before merging back.
