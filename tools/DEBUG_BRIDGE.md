# NXEmu 本地调试桥 (Windows v1)

## 已实现

- `readback-transfers`覆盖Vulkan的Image::DownloadMemory公共落点，包括单层、
  分层、同步、异步、GC、解除映射保留和图像转客体buffer。区分host与device目标，
  避免把图像→GPU buffer与图像→host staging双目标都算作CPU回读。
- 统计的是已排入命令录制队列的区域数据，不是GPU完成或PCIe实测。
  `host_bytes_known`为已知格式下的区域字节；转换/深度等未知字节单独标记，
  `copy_buffer_span_bytes_sum`包含跨度/间隙，不能等同有效复制字节。
- 500ms限频、每个TextureCacheRuntime固定64个图像统计槽、总量不丢；
  top8图像与按原因汇总。same_write_tick只标记重复候选，不自动取消复制。
- `python tools/readback_analysis.py <launch-status.jsonl> 45 75`按新鲜同会话
  累计值差分计算速率；观测缺失/混会话/重置/未知字节保留null，不记0。

- `launch` 外部监督覆盖桥就绪前的退出，保存退出码、原始启动响应、
  启动/加载遥测、日志及可选ProcDump异常转储。不改注册表；独立新bridge目录，
  已有NXEmu进程时拒绝混入测试。退出未知/强制清理与正常quit单独记录。
- `device-backing`记录Host1x捕获与当前OS的base/size、物理表容量和阶段。
  `pre_gpu`可能发现PrepareRom后4GiB→12GiB失配；启动GPU前重建Host1x，
  再由`gpu_ready.matches`验证。history保存切换过程；不把旧事件当实时心跳。
- `events --include-lifecycle`显式纳入同一run此前module generation的
  device-backing历史，并标注`is_current_generation=false`；默认仍严格过滤。
  外部launch监督开启此选项，避免前端进入运行态时generation递增丢掉准备期证据。
  生命周期ring单独有界读取并固定保留在返回额度内，避免被最近的Present事件挤掉。

```powershell
python tools/debug_sdk.py --bridge D:\project\nxemu-switch2-upgrade\new-test\bridge launch --game E:\download\SW2-dumpster\updates\655360\exefs\main --seconds 60 --out D:\project\nxemu-switch2-upgrade\new-test
```

`--debug-bridge <独立目录>` 启用本地后端接口。默认关闭，不监听网络端口。
主线程以约 250ms 间隔写入原子替换的 `status.json`，处理 `command.txt`。
Python SDK 读取真实 `EmulatorSession` / `IVideo::DebugSnapshot()`，无需 UI 点击或截图。

- 状态：模块生命周期、实际 emulation_state、加载/暂停/运行、heartbeat、进程存活。
- GPU：映射调用、已完成映射总字节、正在映射字节、命令提交数、Present 请求数、真实 renderer Composite 完成数。
- 控制：load / pause / resume / stop / quit；通过前端现有公共方法在所属线程执行。
- 关联日志：diagnose 读取有界日志样本，输出 NX2_KIND、NX2_PRESENT、启动里程碑及待调查层。
- 陈旧状态：SDK 检测 heartbeat 超过 3 秒、进程消失、run_id 变化；不把旧文件当成运行成功。
- 请求：run_id + 单调 request_id 防止旧请求在重新启动后重放。目录采用单一写入客户端。

## 启动 (PowerShell，用户手动执行)

```powershell
$exe='D:\project\nxemu-switch2-upgrade\repo\bin\x64\Release\nxemu.exe'
$bridge='D:\project\nxemu-switch2-upgrade\debug-session'
& $exe --debug-bridge $bridge
```

另一个终端：

```powershell
$sdk='D:\project\nxemu-switch2-upgrade\repo\tools\debug_sdk.py'
python $sdk --bridge $bridge status
python $sdk --bridge $bridge command load 'E:\download\SW2-dumpster\updates\655360\exefs\main'
python $sdk --bridge $bridge wait --frames 120 --timeout 120 --progress
python $sdk --bridge $bridge diagnose --log 'D:\project\nxemu-switch2-upgrade\repo\bin\x64\Release\user\log\nxemu_log.txt'
python $sdk --bridge $bridge command pause
python $sdk --bridge $bridge command resume
python $sdk --bridge $bridge command quit
```

## 判定规则

- `command` 的 `accepted` 只是后端收到了请求；验收随后读取的实际状态。
- `emulation_state=4` 表示客体运行；不等于画面生成。`phase=loading` 可以同时出现。
- `present_requests>0, composites_completed=0` 表示画面提交尚未被渲染器消费。
- `frames_advancing` 仅表示 renderer Composite 在增加；黑屏、错误纹理、游戏操作仍需图像校准。
- `Startup: first frame shown` 是前端 UI 帧，不是游戏首帧，不作为游戏验收条件。
- 首次到达里程碑时截图校准一次；以后优先读取 API 和日志。异常或计数/画面矛盾时再截图/抓栈。
- `mapped_bytes_total` 是累计完成的映射字节数，不是当前驻留内存。
- 停止后再次加载曾观察到自行退出；当前迭代建议每次由新进程启动，结束走 quit，SDK会将退出与陈旧心跳区分。

## 当前游戏验收 (2026-11-02)

同一 Pokopia main 已推进到原生 1920x1080 Present。
修复巨大别名链映射耗时和 Present 当前帧同步等待；保留真正的 acquire fence。
固件 22.5.0 已实际安装；最新固件测试 composites=2543，画面仍黑。计数不是游戏可玩证明。
不要将 `frames_submitted` 或接口测试通过表述为游戏可玩。

## 迭代结构

编译 -> 校验 EXE/OS/Video 配套 -> 启动独立调试会话 -> API load -> 等里程碑 ->
API diagnose -> 定点源码修复 -> API quit -> 验证退出 -> 下一轮。
只在初次校准或异常时调用窗口截图与非侵入系统栈。

## 渲染诊断 v2 (2026-11-02)

- 保留 status JSON schema=1，追加 render_debug_version=2 字段。
- 模块 VIDEO ABI 升为 0x812A；必须部署配套 EXE/DLL。
- unbound_methods：真实丢弃的指令字数；dma_batches：DMA 消费轮次。
- draw_requests / draw_pipeline_ready：绘制请求与成功配置管线次数。
- compute_requests / compute_pipeline_ready：计算请求与成功配置管线次数。
- clear_requests / shaders_building：清除请求与待构建着色器数。
- 所有计数属于当前 GPU 实例，读取为 atomic relaxed，无 GPU 等待和 UI 操作。
- pipeline_ready 不证明 host GPU 已执行，更不证明画面正确。
- SDK diagnose 在命令未产生绘制时指向解码/引擎绑定，管线缺失时指向 shader_pipeline。
- 原生截图 API 现有同步等待语义，因此不把它加入 UI 线程的每次心跳。

最新 v2 实测：composites414->509/3秒；draw_requests=0、compute_requests=0，unbound_methods持续增加；SDK指向command_decode_or_engine_binding。runtime-game.png仍黑，退出码0。

## Native sparse readback diagnostics (2026-11-02)

- `physical-alias` rejection diagnostics are sampled at most once per 250 ms per
  reporting thread. `rejected_count` counts all rejected aliases, while
  `published_count` counts publication attempts. `physical_base` and
  `physical_address` support backing-pointer provenance checks. A rejected
  mapping remains a correctness issue; keeping the process alive is not a
  complete address-translation repair.

- Video module ABI: 0x812C. Deploy matching EXE and Video/OS modules.
- SDK inspection and telemetry include `render.native-readback`: `full_download_bytes`, `packed_download_bytes`, `source_layers`, `copied_layer_runs` with base_layer, num_layers, buffer_offset and buffer_size.
- These counters describe the actual planned and submitted texture copy, not a synthetic FPS estimate. The same plan drives fence publication.
- `NXEMU_NATIVE_SPARSE_READBACK=0` disables only compact layer downloads for controlled A/B; all required readbacks and real acquire fences remain enabled. Set it before process launch.
- `stack` also discovers PDBs in the developer module output directories; captures stay non-invasive.
- Current loading-scene observation: 35 source layers, mapped 32-34; 290304000 to 24883200 download bytes; steady loading-logo FPS around 35-37 after startup. This is not gameplay validation.
- The old GPU publisher null-read shutdown path is quiesced before process Close. A later exit still reports 0xC0000374 while freeing Dynarmic JIT patch-information vectors. CPU heap corruption is tracked separately and not marked fixed.
- frame_time_ms is rendering frame time, not measured controller input latency.

For CUA/background launch experiments, pass `--native-sparse-readback 0` or `--native-sparse-readback 1` explicitly. Caller-shell environment alone is not inherited by the CUA daemon. Confirm the actual native-readback document, not the requested flag.

## Native dirty-layer readback and shader timing (2026-11-02)

`--native-dirty-readback 0|1` controls mapped AND written layer filtering; default 1. New images and unknown writers retain all mapped layers. The copy plan is frozen at fence commit; the next write cannot mutate it. `native-readback.json` adds `dirty_layer_filter`, `unknown_write_coverage`, `whole_writes`, and `ranged_writes`.

`texture-header.json` exposes native TIC legacy/effective layer offsets, dimensions, type, and GPU address. Native selected-view addresses must not subtract the Maxwell layer prefix. Legacy TICs remain unchanged.

Existing compiler collectors can now be selected through CLI (also works through a background launch daemon): `--nxshader-timing 1 --nxshader-timing-file PATH --nxshader-runtime-report PATH`. Startup reports split cache read, prewarm, translation, SPIR-V and driver creation. Runtime reports split compiler CPU/wall time, native driver CPU/wall time, waits and disk-cache writes. Startup timing is written after prewarm; runtime timing is written after compiler shutdown, not evidence of live heartbeat.

`physical-alias.json` records rejected physical/device page values and table limits. Physical alias iteration now owns a value snapshot across callbacks; a nested rasterizer callback cannot resize the outer walk's thread-local scratch storage. Unmapped/out-of-table pages do not become buffer-cache indices. This is a bounds/reentrancy correction, not a claim that every mapping or shutdown defect is resolved.


### buffer-transfers (2026-11-02)
Vulkan BufferCache CopyBuffer queued bytes by caller semantic: async/sync download, mapped/inline/index upload, guest/merge/sparse device copies, unknown. DifferentialReadback reports logical ranges separately, NOT actual bytes written by the compute pass. Fixed 11 kinds, thread-safe, 500ms publish cadence; TickFrame publishes unchanged counters to distinguish zero activity from absent coverage. diagnostic_cpu_ns includes record and publication work (current publication appears in next snapshot), not total test overhead. No copy, fence, cache state or barrier removed. Not PCIe throughput. Direct shader/host-mapped reads, texture uploads, query copies and capture utility remain uncovered.

Fallback constraints at actual direct-copy time: disabled, below1MiB, over8MiB, alignment, storage range, buffer over128MiB, budget, eligible-but-fallback. Re-evaluation is diagnostic only, not an exception stack proving a prior compute failure. Multiple conditions use existing eligibility order.

Supervisor shutdown timeout captures exact supervised EXE host stacks with CDB -pvr before cleanup, even when bridge heartbeat has stopped.15s bound; command/result stored in shutdown-stall. Captured stacks are evidence, not a reason to waive normal exit validation. exit_code is refreshed after cleanup; original timeout259 remains exit_before/after_stall fields.


### Segmented buffer readback and retired payload (2026-11-02)
`nxemu.exe --buffer-readback-segments 0|1` / SDK launch `--readback-segments 0|1` allows same-build A/B (default enabled). Vulkan-only large eligible ranges are divided before flags allocation, at existing8MiB cap, preserving source/staging offsets, staging allocation size and FIFO/reference association. Existing eligibility, masks, shader, CPU cancellation and fences stay intact. Failing ranges remain ordinary copies. `segmented_plans` reports logical partitioning, not additional bytes. `retired_payload` reports nonzero flag chunk bytes after fence retirement, including snapshots cancelled for guest copy; flags are actual compute results, not guessed from logical ranges. It is NOT PCIe throughput. Queued direct copies and FIFO-retired payload have different temporal boundaries; do not label their sum precise PCIe traffic. Missing retirement fields remain null.

Fresh Eglantine/eden-nx2 comparison identified missing format43 S8Z24 component tuple UNORM,UINT,UNORM,UNORM. Added exact reference case -> D24_UNORM_S8_UINT; legacy tuple and unknown fallback kept. This is independent from segmentation performance A/B and requires its own post-build runtime validation.


### Input bridge v5
`python tools/debug_sdk.py --bridge PATH input A --hold-ms 100 --gap-ms 100 --count 1`
uses the existing virtual gamepad without altering saved mappings or focusing a window.
Player 0..7, 8 Other, 9 Handheld; buttons A/B/X/Y, DLeft/DUp/DRight/DDown etc.
One nonblocking sequence, max20 pulses,20..2000ms per edge; busy/paused/not-running rejected.
UI loop schedules edges before the250ms publication throttle, no catch-up bursts.
`input` status records accepted/completed/cancelled separately, edge counts and actual
emulated-controller feedback. It is NOT proof that the guest consumed an edge.
Generation changes/teardown cancel without dereferencing retired OS modules.
Validate real menu movement by fresh screenshots; soft keyboard UI remains a separate surface.


### Software keyboard lifecycle observation
`python tools/debug_sdk.py --bridge PATH keyboard [--wait shown|submitted|closed --timeout 60]`
records normal keyboard initialized/shown/callback/returned/exit/destruction and inline callbacks.
Instance ID,mode,frontend kind,min/max length,callback count/result/units; no text content.
Default Windows frontend is an auto-submitting stub, not an interactive popup.
`visible_ui_verified` and `focus_verified` stay null; gamepad pulses do not test a host UI keyboard.
Data is the last lifecycle event,not a live heartbeat. Same-session and event age are separate.
A quick auto-submit can be observed from cumulative shown/submit fields even after return.
Missing event and timeout remain unknown/not-observed,not zero or proof the applet is unused.

`--software-keyboard-name NAME` / SDK launch `--keyboard-name NAME` selects the
default normal-keyboard auto-submit text. No option retains nxemu. User-selected
test name is kknd222. This is not a host keyboard UI, inline support or interactive
editing/validation. Last-submit length/result confirms the callback,not saved name.

`hid-npad-writes` tracks actual NPad shared-memory queue writes per aruid/npad,
sampling numbers,A pressed/released edges,style and skipped updates (1=input
disabled,2=inactive,3=disconnected/None). Fixed160 slots,500ms publication,no ring;
overflow and old-row/current-tick are explicit. Caller holds the existing NPad
shared-memory lock. Guest consumption remains null; no input or LIFO rules changed.
Records cover the system-ext path and the existing style switch before it,not all
HID APIs. Full queue timing and bridge publication overhead still require profiling.
