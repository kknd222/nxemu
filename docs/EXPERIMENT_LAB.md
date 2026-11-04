# NXEmu可复现实验台

## 已有基座
本地DebugBridge status/control、结构化trace、SDK、监督器、匹配模块/PDB归档、显式GPU合成PPM。不是网络服务，不依赖每帧桌面截图。

## 工作流
规格/候选假设→固定参考与输入→记录可判别预测→最小native或ARM反例→真实后端执行→比较→通用窄修→回归。
每个case记录source、reference URL/commit/hash、run/generation、native words、输出与判据。未知输入/未捕获资源明确标注；shader-only归档不宣称完整任务重放。

## 本轮接口
低成本桥、单独trace与完整shader fixture归档开关；集成脚本tools/nx2_lab.py做离线完整性检查和在桥上按需采集，不发游戏输入、不自动GPU每帧回读。确切命令在实现后补写。

## 证据判据
- counter推进只作里程碑，图像与客体逻辑进度另验。
- capture PPM是GPU合成，不是桌面UI或shader输出依赖证明。
- Lower/IR通过不等于SPIRV/GPU数值通过。
- 多同源fork一致不等于独立参考。
- 完整fixture需长度/hash/offset/原四字一致；资源输入/同步/消费者缺失要报告not_captured。
- startup选项须启动前设置；采样默认低成本、有界，重数据显式请求。

## 后续方向
资源依赖关联、任务输入输出捕获、单任务GPU重放、delta reduction、oracle/性质测试、RenderDoc自动脚本和永久回归库。当前待办以项目根TODO.md为准。

## 实际可调用入口
- `python tools/nx2_lab.py run --game MAIN --out NEW_RUN --seconds 30 --detail 0 --shader-fixtures 1 --capture-frame`
- `python tools/nx2_lab.py cases --bridge NEW_RUN/bridge`
- `python tools/nx2_lab.py collect --bridge BRIDGE --out OUT [--capture-frame]`
- 原SDK launch支持5个独立0/1开关；普通CLI启动可只加`--debug-bridge BRIDGE --debug-detail 0`保留低成本基础桥。重启生效，state diagnostic_config是启动配置不是覆盖证明。
- native样本不含SPH/资源；最小真实Lower/IR用例已建立，Vulkan数值oracle与资源重放仍待开发。

## 实际lowerer反例复现与栈分类
- `python tools/nx2_lab.py build-probe --out PROBE_DIR`：编译当前实际LowerToMaxwell，保存源码/EXE hash及编译命令。
- `python tools/nx2_lab.py probe --bridge RUN/bridge --probe-directory PROBE_DIR --out probe-results.json`：逐已核验native样本调用真实lowerer，比较offset/reason；不是IR/SPIRV/GPU数值重放。源码或EXE hash改变要重建，单件1MiB/过程30s约束。
- `python tools/nx2_lab.py stacks --path host-stacks.txt --out summary.json`：有界离线分类104线程等CDB样本；未知不当idle，driver creation/pipeline wait只表示该采样位置，不单凭栈断根因。

## 本轮验收边界
基础桥detail0 30秒正常quit0、56样本；native 3个实际样本在97/7.6/16.9微秒复现HMMA/special拒绝。REDUXOR最小native接受195lowered instructions，真实IR模型测试独立通过但GPU数值未认证。
30秒游戏53compose/208draw/260compute后正常quit0，GPU请求停机前尚未处理，正确cancelled。60秒107样本/96compose/513draw/474compute，显式GPU采集成功RGB全黑4.0109ms；退出等待超过监督器限额，非侵入CDB exit0后forcecleanup15，监督器code4。不能称稳定可玩。
实际Vulkan Report为AMD Radeon(TM) Graphics/AMD proprietary2.0.233；CDB一线程在amdvlk64→CreateComputePipeline，GPUworker在WaitForNativeBuild。是驱动构建等待的证据，不是P106吞吐度量或唯一根因证明。
后续先补per-shader Lower/IR/SPIRV/driver start-end/active阶段与输出规模，复现driver编译耗时并用实测adapter/profile对标；不跳编译或伪成功。

## 耦合实验与编译阶段接口
`--shader-progress 0|1`是第六个独立启动开关；unset随detail，显式0关闭。compute-only阶段观测不涵盖native lower/graphics/cache命中；空active不能判全局idle。前端心跳出版，不靠阻塞driver线程更新文档；wait与driver阶段分别记录。SDK报告文档年龄、观测阶段年龄、历史结束事件年龄和session匹配；已陈旧观测不当连续运行证明。
`python tools/nx2_lab.py run --game MAIN --out NEW_RUN --seconds 30 --detail 0 --shader-progress 1`
`python tools/nx2_lab.py validate-plan --path ../nx2-coupled-progress-20261103/OBSERVABILITY_PLAN.json`
计划验证仅检查DAG/所有权/局部oracle/固定输入/0-A-B-AB四行与显式缺依赖；不执行、也不声称矩阵通过。单修仍黑不否定已认证局部不变量。HMMA/裁剪之间尚无资源版本/绑定/同步因果证据；driver等待还须对应具体program/hash，不能从相邻日志认定它是HMMA。

## 实际管线输入与独立create-only入口
`nx2_lab.py run --game MAIN --out NEW_RUN --seconds 30 --detail 0 --shader-progress 1 --pipeline-fixtures 1 [--pipeline-fixture-key HEX16]`
`nx2_lab.py pipeline-cases --bridge NEW_RUN/bridge --out CASES.json`
`debug_sdk.py --bridge NEW_RUN/bridge pipeline-cases`
`nx2_lab.py pipeline-probe --metadata CAPTURE.json --out RESULT.json [--adapter-match-index 0]`
新开关pipeline-fixtures默认关闭，key需16小写hex。rawSPIRV为实际Emit输出/BuildShader输入，CPU文件捕获不新增GPUquery/submit/dispatch；16case/单4MiB/run16MiB，reload不重置，meta ready最后。typed feature payload只含语义VkBool32，不含pointer/padding；device实际Create链和loaded_extensions，不用supported替代。
create-only helper会自行只读枚举GPU、验证匹配、创建Device/layout/module/pipeline并销毁，外部30s watchdog可终止独立进程，不声称drivercall内cancel。默认多匹配拒绝，explicitindex只作候选。原instanceextensions/设备唯一UUID/应用cacheblob缺失意味着不是完整源应用重放；NULL应用cache不证明driver内部cache冷。没有dispatch，不是GPU数值/可玩验收。
本次SPV171967words通过spirv-val；独立候选0首次21.09s、重复1.56ms，先固定同候选/顺序/缓存再做优化。游戏黑屏正确性仍独立待解决。

## TranslateProgram pass measurement and same-process lifecycle
With `--shader-progress 1`, compute history/active rows add `translation_passes` (22 fixed names, calls/completed/exceptions/wall_ns; null when not observed). Scope is actual TranslateProgram after CFG; native Lower is earlier. Timers do not query GPU or walk IR instructions. Completed pass times are cumulative CPU-thread wall measurements, not startup critical-path proof; graphics omitted. Default profile follows shader-progress. Output SPV hash for actual033c remains unchanged; no numerical GPU acceptance.
`python tools/nx2_reload_probe.py --game MAIN --out NEW_DIR --seconds 30`: existing Cua launch then SDK stop/reset/load, samePID/runid check,20s reset deadline and30s second-run observation, guest-fatal/stale detection, existing shutdown-stall capture. Defaults in ordinary supervisor remain unchanged. First55compose/gen3→idle/gen5→second53compose/gen7, both10/10/no recorded mixed/update drops, normalexit0. Not a game input or long-run/cancellation test.
Current single target winner SSA117.9ms/310.2ms total; aggregate10compute BuildASL1267.5ms andSSA1055.3ms. First test substage/scan counts before choosing containers or insertion caches; all optimizations need actualIR/SPV/semantics/regression and rollback.

## SSA substages and production IR oracle
Compute rows add translation_subpasses(4) and translation_work(23 fixed saturating counters/overflow), under existing shader-progress; child elapsed is included in parent, not added. Use actual existing loops, no per-node clock/log.
`python tools/test_ssa_fastpath.py --library LIB --out DIR [--ssa-source SOURCE]` calls actual productionSSA over7 handcrafted IR cases. Explicit source is compiled, selected library supplies IR support; default lib provenance must be verified with matching SSA/header/library build hashes. MT_StaticRelease required. Raw IR addresses vary; only leading per-instruction address is removed in canonical dumps, preserve raw too. Not GPU numeric/game acceptance.
Prefix checks mean2.39/max87 and opaquePhi/typeBFS0 falsified current two hot hypotheses. Target271105final instruction visits/2950Phi/max2args suggests temporary forwarding Identity traversal to investigate, not proven causal. Rootfastreturn candidate sameIR/SPV and faster high-hit microbenchmark, but targetSSA109.94→116.35ms in sequential observations did not prove benefit. Candidate inactive; tools retained. No claimed FPS improvement.
After rollback copy2 oldermtime, Ninja skipped compilation; touch changed source and verify SSAobject and DLL link before deployment. Do not treat buildexit0 alone as new-code proof. See project evidence nx2-ssa-substages-20261103.

## Phi-prefix Finalize and trace overhead experiment
`python tools/test_ssa_fastpath.py --library LIB --out DIR [--ssa-source SOURCE] [--trace-ab]` now has8 actualSSA cases including16Phi prefix/4096Identity tail/empty block. Full post-pass verification walks every instruction outside timers to validate prefix/types/order. TraceAB adds2warmup pairs and11 alternated on/off pairs over same executable and fixed repeated/dense/sparse IR workloads; construction and census excluded. Record raw paired timings, do not extrapolate syntheticoverhead to game FPS.
Phi-prefix optimization breaks finalization at first nonPhi. SSAFinalInstructions is inspected loop elements (Phis and at most one terminator per nonemptyblock), not total IR size. Production pool uses8192object chunks, operand inlinecapacity2; removed events are not perPhi OSheap allocations. ActualA/B/B/A: visits271105→6142, baselineFinalize31.0888/26.9703ms→0.3687/0.4813ms; identical actualSPV/recipe/loadedbinaryhashes. Translate noise and uncontrolled internaldrivercache mean no general startup/FPS claim. Remaining Visit allocation/Identity chain, crossgame/cancel and blackpixel cause separately required. Evidence nx2-ssa-cost-ab-20261103.

## Guarded seal cache and persistent negative oracles
`test_ssa_fastpath.py --library LIB --out DIR --seal-scaling [--ssa-source SOURCE]`: productionSSA128/256/512/1024dense blocks×16registers;5samples/size, outside-timer construction/census including queryIdentitymaxdepth. Guards only update the cached definition still exactly that incompletePhi and when its replacement differs. No Resolve/cache pathcompression or object removal; laterSet preserved.11positivecases include all scalarcategories/lateSet/successor, aliases and sameUndefobject.10canonicalIR equal; oneUndef alias graph changes but semantic oracle passes.
`... --loop-write-probe`: separate failing oracle using actualPostOrder/SSA, entry7/backedge11. Original and candidate both produce7/self. Store FAILURE.json with exact commands/nativeexit/inputs/hashes/outputs; never count it in positive11. Future successful probe emits validJSON and dumps. Requires realpreSSA gameCFG and consumer linkage before blackpixel causality or a generic filling/sealing fix.
Current realSSA cumulative912/983→633/583/600ms is thread-wall, NOT startupcriticalpath/FPS. Shared7 capturedSPV/recipes equal, fullcaptureonly7/8metadata not10of10. One expandedcapture run guestPANIC(host0/supervisor4), sameconfiguration baseline and candidaterepeat normal. Source points to TranslateHaltReason unknownmask conversion, not known missingopcode. Add bounded rawmask/bitnames/corePC and fixturecapture failureclassification before broader trial. Correctpixels/crossgame/longterm still open. Evidence nx2-ssa-seal-cache-20261103.

## CPU halt return-boundary observation (no mapping fix)
Failure-only NXEMU_CPU_HALT_MAP schema1 logs rawmask/direction/kind/unknownbits, originalmappedvalue, core/architecture/run-step, PC/SP/LR/PSTATE/X0..7, epoch/report-time envtags and ordinal. A32/A64 pass actual ICpuCore; readGetContext only unmapped path afterRun/Step.64 shared events perCPU-module load, not pergame; zero-init/exception isolation marks missingcontext, normal mapped paths do not collect. NXEMU_CPU_HALT_TRACE=0 disables new observer at startup, not original UNIMPLEMENTED or SDK runtime toggle. NCEdefaultcontextmissing not runtimevalidated. Extra parameter-call cost not benchmarked.
`python tools/debug_sdk.py --bridge BRIDGE diagnose --log LOG` and `guest-faults --log LOG` expose typed cpu_halts; report-tag/age matching is historical observation, not activeCPU or ownerepoch proof and does not require video-ready. Definedbit vs unmapped-combination differs; never infer missingopcode from old Unimplemented text. Captured PC is returnedJitPC, not necessarily fault or panicPC.
`python tools/test_cpu_halts.py --source-root ROOT --out OUT [--modified]`: compile actualmapperSource with real enums/interface; assert/log/environment sinks intercepted, contextfake, noISAemulation.2052mask+11forward same,8threads8192stress64unique, off/contextfailure/parser/badrecords/9MiB noncontiguousslice tests. Restored original source tested independently. Preserve realfirst+tail sampledhash but insert parsinggap for omitted middle. Existing criticalassertions not capped by observer.
Thisround baseline+2modified30s normalexit0, loaded5hashmatches, SDKlivefresh/processalive/exit0; no halt orPANIC reproduction/realunknownmask, no diagnosis of prior2296-7227 cause. Toolready is not runtimefaultfixed/Gamepixels/GPUISA acceptance. See nx2-cpu-halt-observe-20261103.


## Pipeline capture outcome audit and independent source review
`pipeline-capture.json` is published at500ms by VideoManager when pipeline-fixtures=1, including detail0.23fixed atomics cover Capture admission/validation/filter/budget/IO and Recipe publication; module-load totals, no pergame reset, nontransactional snapshots. CountersJson does not take archive mutex, avoiding self-induced capture rejections. Disabled calls are not sampled; preCapture shader failure/cachehits/graphics are outside coverage. ready means input metadata published BEFORE drivercreate, not driver/GPU success. ReleasedWithoutRecipe is handle lifetime, not task outcome.
SDK inspect/render and pipeline-cases expose the document even with no ready archive; typed invalid counters rejected, session/freshness explicit; pending_only is missingreadyfile evidence, not active build. Actualheader test tool `python tools/test_pipeline_capture.py --source-root ROOT --out NEW_OUT [--modified]` uses real filesystem/concurrency/budgets with syntheticenvironment/dummySPV, noGPU. Use fresh OUT because deliberate failure artifacts are retained.
Actual allrun attempted10/ready8/lockcontention2/otherrejections0; filtered033c attempted10/filtered9/ready1/drop0. Shared8SPV/recipe and targetSPV unchanged, no new pixels. Next reliable short reservation and explicit singlekey preSSA paired capture, rather than force blocking IO or claim all10captured.
Latest source audit: Yuzu7ffac53c historical, Eden8d29148a, EdenNS2d21b2861, Eglantine171f9854, MeloNX6a1c1596. Eglantine old/new3SSA files unchanged. Ryujinx dominance-frontier+twopassSSA is independent candidateoracle; actualbackend not yet executed. Capture immediately before/after TranslateProgram SSA, not finalSPV; preserve orderedCFG/postorder/fulltypedIR. New Eglantine sharednativeLoweredCode and GPUwrittencommandmirror are hypotheses needing sourcehash/lowercounts and writer-consumer version diagnostics. Preserve localgpentry SYNC_WAIT/fence semantics, validate resource visibility and invalidation; NS1compat does not make allNS2input Maxwell-only.


## Run-owned short reservation and independent SSA oracle
Capture hashes and disk IO outside short trylock admission; run-owned shared chargedbytes include in-flight writes, retained by noncopyable BlobCharge. Failed blob refunds own run; pendingfailure after blob commits charge. Same run reload keeps limits, arbitrary A/B/A run identity is not one persistentbudget guarantee. IDs retain old no-run format; normal onebridge/run root remains required. Disabled nohash; enabled budget-rejected captures currently hash before admission, saturation-overhead improvement remains planned.
`test_pipeline_capture.py --source-root ROOT --out NEW --modified [--reservation]` inserts test-only IO gate into original Write body; realfilesystem controlled4large/16tiny in-flight limits and crossrunrefund, no unmodified timing/GPU claim. Successful admittedbody timing excludes accounting/unlock/rejections; max is best effort. Twoactual30s run10/10zeroerrors, newrepeat10SPV/recipes equal and oldshared8equal, not allbaseline10. FirstComposite16.75/17.078s means only13s postcomposite observation. No currentpixels/gameplay/FPS/longterm acceptance.
`test_ryujinx_ssa.py --reference-root PATH --out NEW [--negative-expectation]` compiles16 actualRyujinx shader IR/CFG/Dominance/SSA files using localRoslyn+.NET8runtime; four handIR variants expected7/11 or7/12, negative native1/reporter0 distinct. ActualCFGconstructor not decoder/linearCFG.Create/NXEmuBuildASL/GPU. NewMeloNX coreSsa/Dominance matches latest6a1c1596; full16latest identity not claimed. NXEmu currentSSA source override probe still[7,self]/assertfastfail native0xc0000409 outer0xffffffff, not live emulatorcrash. Use confirmed semanticdifference to guide realpreSSA capture, not immediate wholesaleSSA replacement. CurrentEglantine7fbaea9 threeSSAfiles equal prior171f9854.

## Actual paired SSA-only fixture
Start with `nx2_lab.py run --ssa-fixtures 1 --pipeline-fixture-key 033cdd2f05959e77 ...`; default0, explicit one key mandatory. Capture1pair/module-load,64MiB/phase128MiB/pair, reload noreset. Encode strict parse-only validation; no second production pool. Ordered CFG/postorder,raw Value5slots,stable objects/Phi/cache/sealed/use/pseudo/definition preserved; no Info/syntax/environment/GPUresources. Old32MiB failure retained; actual post34.49MiB justified boundedincrease.
`test_ssa_fixture.py --library LIB --out OUT [--pre PRE [--post POST]]` compiles actualcodec/SSA/capture sources and production IRsupport. Preonly explicitly not paired acceptance. Actualpair roundtrip/replay byteexact; sameimplementation replay is reproducibility, not independentcorrectness. Actual3selfPhi entry-only values valid; do not patchSSA from unrelated handIRdefect. `NxEmuSDK.ssa_cases()` / `debug_sdk.py --bridge BRIDGE ssa-cases` reads explicit bounded archive with sessionmatch/historical tags; not each telemetry heartbeat.
60second tests distinguish perrun93stalled vs779/full60FPS; frame_time16ms not inputlatency. Explicit GPUcapture near36.276seconds allblack; later FPS/frames not evidence of laterblack or correctpixels. Capture GPUfinish perturbs timing. Next pixelseries and actual output→sync→consumer→composite chain; no visual/gameplayfix claim.

## Scheduled offscreen pixel observations
`nx2_lab.py run ... --seconds 60 --capture-frame --capture-at 30 --capture-at 50` enables ordered1..3 captures through actualSDK. Default empty schedule keeps legacy onecapture branch.8s timeout+2s shutdown margin,12MiB imagebound; skip stale/0composite; bindsession onfirstcapture; stop onfirstnotcompleted (timeout does not cancel GPU request), do not overwrite inflight. `test_frame_series.py` uses real scheduler/fakeSDK; not GPU correctness.
Capture is separate blit_capture recomposition from sameframebuffers, NOT presented swapchain image. Actual30.187/49.981s outputallblack; Cua observed49.985s nativewindow black too. Composite counter counts returnedcalls including hidden/hold, not GPUcompletedframes. TwoFinish perturbtiming; do not label performance regression or FPSfix.
Current GPUinstruction rejections remain active: S2UR30/31,HMMA,local scalarspill,LEAcarry/high. Fresh errors prove rejectedinstruction paths, not precise blackpixel producer-consumer. Before arbitraryconstant/stub, capture actualinput/identity/state and validate truebackend. `resource_trace.h` is Loader/FSPhistory, not GPUwritertrace. Next request-bound ImageId/ViewId/layer/role/version/returnpath and source-vs-actualframe pixels.


## 2026-11-03：请求限定的合成资源关联完成，双时间黑像素复测
- frame_capture_context.h只在已接受frame capture ticket期间启用TLS；16KiB、每role4层、16来源；记录输入、cache ImageId/ViewId、owner、write/modification ticks、实际descriptor/crop/output与return_path。SDK frame_context(request_id)及capture_frame可选context已集成；缺/损坏context不丢有效像素。未加GPU调用或新Flush/Finish，未改shader语义。
- 实际header默认off/TLS/8线程/边界/IO/异常测试及实际SDK fakeGPU publisher+真实PPM文件测试通过；这是工具正确性而非GPU数值认证。10步Release编译exit0，四输入hash匹配，部署10个EXE/DLL/PDB归档hash已重验；repeat-run未采实际加载模块hash，不冒称本轮loaded5复验。
- 首轮30/50调度仅一采样，第二因最后发起deadline稍迟跳过。复测30/49得到约30.125与49.453秒两关联context，均offscreen全黑；同run image1/view2/source_view6059483968，ordinal45→248、gpu_write_tick894→5157；offscreen和swapchain均同图/full crop、present_enqueued、非held/hidden。这证明选图/提交路径，不证明源纹理像素或实际Present完成。
- 复测60秒108采样normal_exit/exit0/no forcedcleanup/no dumps。FPS/客体speed变化不等于正确画面；黑屏未修复。SSA真实重放结论及native指令拒绝仍保持，勿使用任意常量/桩。
- 下一工具前置：present/util.cpp CreateWrappedImage未声明TRANSFER_SRC而既有DownloadColorImage调用CopyImageToBuffer，先修用途并验证；配对源与实际frame.image像素应在DrawToFrame后、Flush(render_ready)/Present移交前；不能在Present后后台下载复用句柄。Image::DownloadMemory会ScaleDown/ScaleUp，不能当descriptor当前scaled源采样；需要精确format/mip/layer/sample/layout及cache锁下实际选图元数据，限额/同步/生命周期测试。
- context publication是renderer scope exit，不是GPU完成。slot generation/writer shader/source pixels/target pixels仍缺；Vk handles只限同进程。原件8/新增4、四件与独立副本rollback见nx2-capture-context-20261103；源码/诊断部署保持修改，未Git提交/推送。不发游戏输入，不改ROM/keys/固件/存档/驱动。


## 2026-11-03：修正读回图像usage与恢复访问掩码
- present/util.cpp CreateWrappedImage增加TRANSFER_SRC；DownloadColorImage恢复GENERAL的dstAccessMask增加MEMORY_READ，保留WRITE。未新增flush/finish、不改shader/cache语义。新增tools/test_present_readback_contract.py仅验证源码契约，不冒称GPU执行测试。
- 五步实际util.cpp编译/库/DLL/EXE重链接exit0；匹配部署10件，运行中psutil memory_maps核验EXE+4DLL共5路径/hash一致。第一次deploy因UTF8 BOM失败，已由SDK quit中止混版本live-run（early_exit/host0/supervisor4）；该run剔除，deploy.py读utf-8-sig修正后重新部署verified-run。
- 新60秒verified-run正常退出exit0/no forcedcleanup/no dumps；约30.156/49.125秒两采样仍全黑，合成ordinal52→269。这排除图像usage缺陷作为本次离屏黑像素唯一原因，不证明shader根因、实际源黑、实际Present完成或可玩。
- 下一请求专属源/实际输出配对：DrawToFrame后/Flush(render_ready)前读frame.image；AccelerateDisplay cache锁下记录实际scaled backing/format/swizzle/mip/layer/samples，在AA前复制，不调用会ScaleDown/Up的DownloadMemory冒充当前源。仅支持明确单样本RGBA/BGRA；限额、同队列layout恢复、所有权/超时/退出、真实2x2颜色与非零layer/缩放回归先验，再游戏采集。尚未实现该工具。
- 原件5、新工具1与独立源副本rollback/四件见nx2-readback-usage-20261103。当前部署保持修正版，黑屏仍开放，未Git提交/推送；无游戏输入、ROM/key/固件/存档/驱动改动。


## 2026-11-03：同请求源纹理/真实合成输出配对，黑屏边界前移
- 新启动开关--paired-pixels 1/NXEMU_PAIRED_PIXELS=1默认off，nx2_lab.py run --capture-frame --paired-pixels --capture-at 30 --capture-at 49启用；已接受原frame-capture请求每run4上限，不新增每heartbeat读回。源首层最多8MiB，target最多8MiB；两复制同Scheduler队列一次新增Finish，原离屏Finish保留。pair原始RGBA+PPM+alpha/black统计，manifest最后出版，原Complete延后到pair publication之后；SDK paired_pixels(request_id)/capture结果同请求关联，坏/缺pair不丢legacy。
- 源在AccelerateDisplay cache锁内选中实际scaled backing/Color2D view，具体mip/layer/format/sample；不调用DownloadMemory、ScaleDown/Up。实际VkImageViewCreateInfo.components仅成功Color2D创建后记录，不伪造unknown。支持已知非identity0..6映射；raw/PPM未swizzle，view派生统计只是encoded通道零/一分类，不是shader/SRGB线性数值。scaled非零mip、MSAA、未知映射/格式等明确unavailable。输出在DrawToFrame后/Flush(render_ready)与Present移交前复制renderer-owned frame.image，不宣称OS swapchain pixels。
- 工具审查修两P0：Finish可能吞deviceLost返回，现检查device.IsLost，不将其标gpu completed或读未写buffer；失败wait区分drained/数值completed。wait异常最多2buffer移交renderer固定array并禁后续pair，array在Scheduler前声明保证CPU recording停止后释放；默认析构原Finish/WaitIdle保留。DeviceLost/wait异常未动态注入验收，不称该故障稳定性已结案。
- actual helper+actual SDK28场景与原SDK基线/独立source rollback通过；真实文件系统/symlink3、RGBA/BGRA/packed、alpha/黑色、8MiB/overflow、IO、session、坏payload、JSON bool/float混淆、未知/非法swizzle及派生统计永久反例。fake GPU publisher测试不是GPU数值认证。独立真实Vulkan2x2两array层六格式37/43/44/50/51/57上传→读→GENERAL→再读逐字一致，KHRONOS validation0error；2 loader错误来自失效Steam JSON且未改系统；独立设备选择不是模拟器adapter身份，不包含实际shader/缩放/绘制。
- final19步相关CPP/生产库/videoDLL/EXE实际构建exit0，9输入hash前后一致，10匹配EXE/DLL/PDB归档部署，运行中5模块hash匹配。final60s105sample正常退出exit0/no forcedcleanup/no dumps；默认off30s53sample正常退出且pair files0。不是跨游戏/长期/取消准备验收，无FPS收益声称。
- 关键新证据：约30.359/49.062s，两源1280x720 format51 rawRGBA全部0，实际components[3,4,5,2]=R,G,B,ONE使viewalpha255；两target1215x647 format44 RGB黑/alpha255。ordinal30→187，source cache ImageId1/ViewId2→ImageId2/ViewId5，writeTick533→3893，源/descriptor/输出handles分别与同请求context严格匹配。两个buffer轮替且都黑，排除仅合成/隐藏/旧frame被挡的解释；只证源内容先黑，不证明writer shader根因，也不能排除原图→缩放图/cache visibility问题。
- 下一P0沿选中资源反向串writer操作/原图与scaled图版本→实际color-target/clear/copy/dispatch/draw→shader key和拒绝事件；按同owner/对象代际/内容hash锁定首次分歧，不盲放行S2UR/WSCALE常量、HMMA桩或其它拒绝。first intermediate run源nonidentity unavailable已被mapping-aware final复测替代。
- 原14/新5、四件/精确命令与独立source rollback见nx2-paired-pixels-20261103。当前源码/部署保留工具和累计语义补丁；黑屏尚未修复，无游戏输入、ROM/keys/固件/存档/驱动改动，未Git提交/推送。NewLai GET /api/projects401已记录，未改平台库。


## 2026-11-03：有界图像写入记录及原图/缩放图关联
- 新--image-writers 1/NXEMU_IMAGE_WRITERS=1默认off；lab需--capture-frame --paired-pixels --image-writers。512槽/4探测/每槽8历史/最多4节点/64KiB；birth epoch在enqueue时固定，worker记录前校验dst代际，避免VkImage复用污染；src代际保留，父已重生/未知/驱逐明确停止。trylock遗漏/代际失配/unknown/驱逐/overflow计数；不加GPU查询/Flush/Finish，按已有paired请求采样。默认关闭原Draw/Clear closures逐字相同，新增对象大小/分支及开启开销未做全游戏性能基准。
- 插入original/scaled Birth、普通Upload/Copy/BlitScale的实际worker命令record、scale ignore仅标无复制切换、helper仅queued而不冒充实际写。pipelineConfigureDraw真实ColorWriteMask、压缩attachment rt_map+scratch映射、固定pipeline key及6stage hashes；直接Draw worker区分实际Draw与unbuilt skip。native全RGBA通道Clear记录实际4word和矩形/层（全通道不是全表面）。不修改GPU指令/纹理/绘制语义。
- SDK image_writers(request_id)和capture结果字段关联既有pair/context，坏/缺writers不丢旧像素；metadata在existingComplete前出版，scope为CPU command-record history。父节点保存当前历史，不按child copy.seq截断，所以不是完整producer DAG，解释须排除晚于复制的父写入。8历史截断及未覆盖路径不得当无写入。
- 实际header/SDK46场景+原SDK基线通过：默认off10000操作、启动开关独立进程、8线程320k CPU操作、dst/src出生重用、8环、碰撞/驱逐/4node/cycle、14JSON、request/session/schema/type/seq/birth/clear/path/budget/坏记录不丢legacy。GPU未执行；serial饱和仅源码契约。独立15source copy rollback移除new3并原SDK回归，live源码/部署保持修改。
- 初次编译暴露Scheduler要求const-callable闭包，RecordWriter mutable已去除，不改原body；初失败build.log保留。fixed实际obj/库/DLL/EXE及clear矩形版final14steps成功exit0，final9输入hash前后一致，10匹配EXE/DLL/PDB归档部署；final运行中5模块hash匹配。两个60s writer run均107sample normal_exit0，无forcedcleanup/dump；final默认off30s正常退出且writers文件0。未做跨游戏/长期/取消与开启FPS基准。
- 新实测：约30.172/49.297s同请求source1280x720和target仍黑，writer snapshots与pair/context exact source handle/ordinal匹配；两个source retained last8均native clear bits[0,0,0,0]，区域[0,0,960,540]、layer[0,1]，不是整个1280x720。第一source events_total14，第二102；对应2560x1440父original各1条upload。snapshots lock/stale/missing/eviction0。保留历史没有direct Draw，但不能外推所有早期/未覆盖写入从未存在。优先假设是清除后显示目标的实际draw admission没有通过，仍须精确目标关联验证。
- 同run Native shader 0x11c1c1ba00+0x10反复special register opcode0x9c3/words000479c3,00000000,00001e00,000e7000（SR30）拒绝。尚未把此失败与目标绑定；下一补pipeline CurrentGraphicsPipeline失败之前的guest RT owner/目标范围和shader admission身份，再跟目标实际record链，而不是直接放行常量/跳指令。当前工具漏ConfigureDraw前失败明确列P0；不把draw_requests或dirty ticks当实际写。
- 最新只读参考HEAD Eglantine46967a1b84e857a783ace6dcbabffddd008463ff、Takoroka a2aa4d97e10a8345babfe647e1abd5ed5909c623、OSD d21b286125fcc2bcf10295feda60d16d98ffa8be；11小文件HTTP200、UTC/hash与静态差分见reference/REPORT。非2D高度修正不解释本次2D黑；opaque R,G,B,ONE为预期。保留本地mapped-layer增强，不盲抄选择器；helper失败回退/current backing是候选待验证，原图>8MiB可用严格限定ROI而不扩大黑ROI结论。
- 四件与精确命令/输入/输出/exit在nx2-writer-chain-20261103；原15/新3，当前源码与部署保留工具/累计补丁。黑屏尚未修复，不发游戏输入，不改ROM/keys/fw/存档/驱动，未Git提交/推送。


## 2026-11-03：PrepareDraw拒绝与黑显示源关联，真实22指令反例
- 新draw_admission_trace.h沿已有--image-writers/NXEMU_IMAGE_WRITERS=1启用，默认off。PrepareDraw调用管线前只读raw RT(output→slot、color mask、format/tile/base/depth/array_pitch)和registry已有owner/birth；不额外解析/创建Framebuffer，不新增Flush/Finish。256owner槽/4probe/每owner4历史+first rejection、8before-call bindings、最多20packet/32KiB；trylock/unbound/binding overflow/出生失配裁剪显式。first旧birth不后补绑；first被ring覆盖独立保留，重生后的新bound first可替换旧槽。
- RefreshStages TLS hook只填本次resolved program/已成功hash；dirty shortcut明确cached_valid/invalid，失败不读取混合旧unique_hashes作key。完整Refresh及state/cbuf后才MarkKey，为requested_pre_lookup_key而非最终normalized pipeline身份。native失败同时记录原opcode/words/stage/reason；保留原throw/reject/dirty/cache语义。区分pipeline_null、async_skipped、wait_failed、pipeline_ready、exception_aborted；asynchronous lower在TLS外仍未覆盖。
- writers.json新增admissions，依同request/run/generation/ordinal与writer/pair关联。实际hostattachment尚未Configure，所以只能称guest owner+beforecall缓存birth候选，不当实际GPUwrite。admission序列与worker记录不同CPU域，不能按两个serial排序。rawwidth可能bytepitch，array_pitch单位不换算成猜测范围。admission序列化失败保留原writer，SDK坏/缺admission保留valid writer及pixels。
- actual headers15JSON、8threads16000scopes、TLS/off/代际/first/ring/budget/startup反例通过；旧writer14JSON/8threads320k及SDK46回归通过；新reader27场景/14实际CPP JSON通过（maximum为故意非语义size夹具，已拒绝）。CPU/file fixture不冒称GPU执行；完整16steps Release build exit0、7输入hash前后一致、10EXE/DLL/PDB匹配部署，两个运行各loaded5核验。
- 两60s启用run为108/102样本，第三默认off30s55样本；全部normal_exit0/no forcedcleanup/no dump；off writer文件0。首个启动试验为Cua daemon不运行（observer_error，未启动NXEmu），启动会话辅助daemon后重跑；不是游戏闪退。NewLai/api/projects401留证、未改数据库。启用诊断/Finish影响时序，本轮不作FPS或开销A/B结论。
- 核心：两个run四次约30/49s源1280x720 rawRGBA全0、实际renderer target RGB黑；每次selected source及parent original birth关联5个PrepareDraw事件，VertexB(stageindex1/nativeStage0)全pipeline_null/refresh exception，S2UR opcode0x9c3 words000479c3,00000000,00001e00,000e7000，key_valid=false。首run采样末binding lockmiss3/unbound4，非零遗漏不隐去。显示guest目标的draw admission被该指令阻断已证实；尚不证明这是唯一黑屏原因或修SR30即能出画面。
- native-fixtures显式捕获8unique共102576B；同native run的10admission包与352B/22instruction shader按program/stage/opcode/offset/words/run/generation精确匹配（NATIVE_ADMISSION_LINKS.json），不跨ASLR地址串因果。真实当前Lower probe重放8拒绝全部同reason/offset，目标SR30+0x10复现；只为native decoder/lower反例，不含SPH/cbuf/resource/sync/数值oracle，不称完整GPU重放。
- 下一P0：沿该22指令UR4←SR30/UR5←SR31的消费者做位精确use/def切片，核验WScaleXY/Z真实数值与阶段/graphics state；参考最新Eglantine允许30/31但后端0x00ff0000 STUBBED，以及本地旧0x3f800000均不是硬件证据，不只放行未知值。确定规格/独立oracle后做真实Lower→IR→SPIRV→GPU最小实验和通用窄修，再跨游戏回归。保留SSA有效真实图结论及其它HMMA/localspill/LEA拒绝候选。
- 四件/原11新3/独立源码rollback（恢复11移除3并原header/SDK重测，主树14修改保留，非live binary rollback）见nx2-draw-admission-20261103。当前部署为诊断版，黑屏未修复；未Git提交/推送，无自动游戏输入、ROM/keys/fw/存档/驱动修改。

- 同一352B/22指令的静态消费者进一步确认：0x40/50 LDC写普通R6/R7与R4/R5，不覆盖UR4/5；0x60 FMUL R1,R7,UR4和0x70 FMUL R3,R7,UR5实际消费SR30/31，后续FSETP与EXIT形成clip/culling链。所以这两个读不是死初始化，不能以DCE删除。0xf0/100还存在CSMTEST opcode0x80d，本地未支持，最新Eglantine以not-modelled false stub处理；正确WScale后仍有CSMTEST/P0 EXIT语义待解，列成耦合前置，非把SR30当唯一修复。静态切片由sr30-reference报告保存，硬件/独立数值oracle仍缺。


## 2026-11-03：CSM/clip模型证据、真实bank1输入与LDC.IS通用窄修
- 新native-draw-inputs显式启动开关（默认off），要求shader-fixtures/image-writers/paired capture；Graphics失败前保留真实native stage身份。先Reserve再新增ReadBlock，module-load最多4，不按reload重置，失败/重复不退款；原native≤4096、raw SPH128/parsed真实80、regs14336、cbuf prefix≤1024，5 raw文件后manifest最后发布。header缺失不造padding、unmapped不补zero；SDK fnv/SHA、有界路径校验、historical/live_state分开。actual capture17场景、SDK34/20actual CPP JSON通过，CPU/file fixture不是GPUoracle。
- 纠正旧探针：22指令起始ALD实际R0=VertexId，RZ是地址寄存器；LEA R0=(VertexId<<5)+0x10000。三个LDC64是IS2模式，高16选择bank、低16offset，不能以bank0 prefix推断Position=0。旧input-run/LIVE_SUMMARY保留并标superseded；新probe只对明确匹配的VertexB recipe抓bank1，仍未观测实际VertexId/base-instance和完整GPU可见性。
- native Lower的LDC32/64允许Default0和IS2，Maxwell mode bits44:45保留2，交既有真实Slot实现 uint32(Ra+signedoffset)、bank=base+(addr>>16)、offset=addr&0xffff；IL1/ISL3及未知modifiers仍拒绝，IS rawbank≥18不沿用Default bank24→17 alias。真实Lower→生产TranslatorVisitor::LDC/Slot→实际SSA/ConstantPropagation：baseline native_ok232/IS拒绝216，modified native_ok448/IS拒绝0，两端地址IR各447、坏输入17、Default编码清单hash相同。pred仅验证编码，未执行predicate/GPU/SPIRV或整ROM重放；手工oracle不是硬件证明。
- 新bank1-build实际Release成功，8输入前后冻结一致；部署10EXE/DLL/PDB，bank1-run运行中5加载模块hash匹配。60s 107samples/30s默认off 54samples均normal_exit0/no forcedcleanup/no dumps；默认off无input目录。运行中SDK available/fresh/process_alive true、exited false已取证，不把退出后历史数据当live。
- 同run4inputs bank1 enabled/mapped，declared65536/prefix1024，前三record为(-1,-1,0,1)、(-1,3,0,1)、(3,-1,0,1)且带UV，符合全屏三角形候选，未观察实际VertexId则不能称真实已执行位置值。约30.453/49.344秒两个source和renderer target仍黑，精确同token/program/stage/error words关联2个input，SR30仍在VertexB+0x10阻断PrepareDraw。reserved/claimed/ready4、raw63680B，budgetdrops25→197，其它错误0。显式Finish改变时序，不作性能提升声称。
- CSMTEST公开sm75 ISA pin b46f0b19：+f0 RA vote_index是28bit 0x04321111，不是32bit c4321111；+100 A.FCSM_TR P0,0→@P0 EXIT。NVIDIA专利支持提前顶点clip-status路线，未证实批次状态转换/边界。0x193c raw181d的XY256/Z1是guardband枚举，不是SR30/31返回值；Turing Z range bits17:16与本地legacy bit0分开。TF关闭但userclip0xff、zpass query1、zcull stats1，lateclip规范化前置未齐。Eglantine SR常量/CSM false均桩，未移植；SR乘积即使后来R1被LDC覆盖，CSM/P0仍有消费者。
- 参考CSMTEST/WSCALE/CLIP_GUARDS及ISA/专利URL/ref/commit/UTC/hash保留reference；最新核验pins Eglantine46967a1b/Mesa322ab694/Envyf102b823/NVIDIA9fdf5c40，本轮未再盲pull。黑屏仍开放，当前只验局部LDC寻址与工具；未Git提交/推送。四件/15原hash+5new及独立源码回滚见nx2-csmtest-model-20261103，主线20修改和新部署保留，非live binary rollback。

- 下一P0：用实际bank1数据、真实执行VertexId/primitive/batch及可观察query/clip-distance定义最小early-clip行为模型；通过独立/硬件证据求SR30/31数值与CSMTEST状态，先真实Lower/IR/SPIRV/GPU局部验再做0/A/B/A+B。不得为整游戏仍黑回退已通过LDC局部语义，也不得用常量/false/跳EXIT掩盖未知。
- 若观测工具欠缺，优先补有界VertexId/base-instance、primitive/batch、clip/query状态或最小客体入口；避免全量每帧GPU读回。之后联合黑像素→writer→admission因果复测，跨NS1/Pokopia/Kirby、取消/重载、长期稳定/性能仍待。
- 补充永久回归：两端447个地址IR检查还核验postSSA最终SetRegister消费同一GetCbuf，64位extract lane0/1；5个@!PT由原有Lower静态去除且不调用visitor，217个Default编码逐字一致。早期fixture assert fastfail已保留，不是模拟器游戏崩溃。


## 2026-11-03：实际draw invocation与有界索引输入闭环
- 新native_draw_invocation.h在Vulkan Draw/DrawIndirect入口TLS Scope记录实际caller indexed/instance_count和DrawManager retained topology/mode/index/vertex/base/inline前16字节；default双开关off，nested/异常/thread隔离，非法Snapshot原样交capture拒绝，不伪成unavailable成功。indirect count未解码，retained_instance_count不当实际caller参数。manifest.invocation及index_input进入唯一repo/SDK；same native/regs/cbuf但不同invocation/index不错误dedup。
- 首轮新观测确认为Direct indexed16bit、Triangles4、index_count3、first0、base_index/base_instance0、实际instance_count1；retained_instance_count0只是其它状态。旧vertex_first/count寄存器不能推出非索引调用。随后在既有Reserve后增加最多16B CPU index prefix；module-load仍4slots/no reload reset，无新增GPU Finish/Query，每slot额外最多16B；五raw文件charged计数不含JSON索引数据，另行说明。
- 首次索引limit.EndAddress0导致原探针明确range_validfalse/未读。已修诊断契约：zero limit是未知并原样保留0，count/address需无uint64溢出，非零limit继续约束，仅FullyMapped才ReadBlock；不把unknown limit伪成合法硬件binding。当前4case均6byte [0,0,1,0,2,0]→CPU index[0,1,2]，精确引用bank1前三个Position(-1,-1,0,1)、(-1,3,0,1)、(3,-1,0,1)。GPU资源可见性/实际执行VertexId仍未认证，shader仍在SR30拒绝。
- actual helper/DrawManager/捕获18场景/16坏index、TLS8thread/默认off/nested/invalid/inline预算/dedup通过；reader47场景、旧capture17、旧SDK34回归exit0。早期reader mutation误把empty变empty、Python manifest selector夹具问题保留；生产index编译缺DrawManager完整include已窄修并保留失败log，不是游戏崩溃。
- 最终Release构建4生产输入冻结一致、匹配10EXE/DLL/PDB部署，运行中加载5hash及SDKfresh/process_alive/cases4取证。final60s 108samples、defaultoff30s 55samples均normal_exit0/no forcedcleanup/no dumps，off无input目录。约30.000/49.484秒两源/合成仍黑，2个同token/program/stage/errorwords→索引/输入→黑display-source admission链。未改shader语义、不声称FPS收益/游戏修通。
- 最新参考核8HEAD；Mesa及sass-document更新，sm75 ISA blob未变，Eglantine仍false桩。Vulkan官方规范按primitive裁剪，clipper计数可观察；不能只凭无TF/SSBO或颜色相同推出earlyclip等价。Nsight当前shader debugger target至少Ampere，debug/JIT可能移除原优化，必须分别冻原optimized/debug SASS确认SR/CSM仍存在才做oracle。URL/ref/UTC/hash及31产物见reference/LATEST_SEMANTICS.md。没有发现精确SR30/31、R/A/RA/Sa/FCSM/batch状态合同。
- 四件/原8hash+new3与独立源码rollback见nx2-clip-invocation-20261103；唯一主线11修改保留，非live binary rollback。无游戏输入、ROM/keys/固件/存档/驱动修改，未Git提交/推送。
- 下一P0：利用已固定index[0,1,2]+bank1 Position，建立真实Vulkan标准lateclip/primitive/query最小实验与SPIRV资源/VertexId路径回归，区分基础clip实现与CSM隐藏状态。其通过不是跳unknown CSMTEST授权；优先独立graphics优化后SASS/硬件oracle，记录adapter/driver/原/debug hash及批次边界。
- 继续验证actualGPU binding/visibility、匹配实际pipeline clipping/depth/viewport/query生命周期、SR30/31数值与CSMTEST状态；工具缺口先补有界可重放入口，不扩成全量每帧日志。局部LDC.IS回归保持，联合0/A/B/A+B、跨NS1/Pokopia/Kirby、取消/重载/长期/性能仍待。


## 2026-11-03：真实前端到 AMD Vulkan 的标准裁剪最小实验
- 永久工具 tools/test_native_ldc_spirv.py、tools/test_vulkan_clip.py 已进入唯一repo；外部运行、无默认游戏开销。原352B shader只抽取ALD/LEA/LDC/AST/EXIT六指令组成合成入口，不修改游戏shader，不宣称原SR/CSM兼容。
- 实际Lower→生产Translator/SSA/ConstProp/DCE/CollectInfo→SPIRV；旧Lower IS拒绝，新Lower 7词/2动态cbuf loads/4 Position stores/14UBO；SPIRV993词 hash cb305f861d6c4bd1e5fb7ed1d8f566a7f61e95692159d4fe28cd3e0eae37170f，spirv-val通过。
- 新鲜复测独立GLSL与真实前端SPIRV在显式adapter0 AMD Radeon(TM) Graphics：full/outside/mixed像素与精确occlusion分别256/0/224，像素hash及全部查询统计一致；Vulkan validation errors0，2条Steam JSON loader缺失单列。启用SPIRV实际要求的ImageGatherExtended与DrawParameters，早期08740失败保留；不改SPIRV隐藏错误，不冒称选中设备等于模拟器adapter。
- 软件query审计 SOFTWARE_ROUTE.md：真实ZPass64会回写guest并影响conditional；zcull/VS/clipper stats目前fallback1，不能当硬件oracle。此轮独立hostquery不是生产guest闭环，原CSM的“EXIT只拒绝晚期必丢primitive”合同仍未证明。
- 下一P0：真实QueryCache Enable/Draw/Report/Reset/分pass/guestwrite/conditional闭环；仅确有consumer疑问再补32事件窗口。优先标准clip模型与编译器保守裁剪合同，未知SR/CSM不以常量/false放行。继续每轮核验快速更新的NS2参考HEAD、旧pin、新源码hash；同源fork不是独立硬件证明。
- 证据 nx2-vulkan-clip-20261103：新鲜baseline/modified及独立副本source rollback，修改4docs+新增2tools；此前LDC.IS生产修复和EXE保留，此轮未修改/部署模拟器二进制、未启动游戏、未Git提交/推送。Kirby黑屏仍开放，完整shader/整游戏/跨游戏/长期性能待验。


## 2026-11-03：快速更新参考清单与query邻项查找窄修
- 每轮相关开发先核远端HEAD、分支、release tag实际commit，再只取变化的目标源码并保留旧pin/hash/UTC；不按build数字选择新源码。固定关注Takoroka/eden-nx2、Djihads80/Eglantine nx2及AMD轨道、sskyNS/Eden-NS2-dlss-osd、0x2593/Eden-NS2、Wael3rd/Eglantine-MKW。
- 最新刷新 nx2-reference-refresh-20261103/reference：5仓库/41文件hash通过；Eglantine46967a1b、ssky d21b2861、MKW2108c0ea、Takoroka a2aa4d97未变，新0x2593 b2cef0c0。build178是旧experimental ec667e25重编，主线build177为46967a1b；Takoroka fixed raw403单列。现有CSM false/WScale STUBBED仍非语义答案；四分支query同hash非独立oracle。
- P1候选：descriptor单页memcpy/跨页fallback及热日志TRACE、lowering复用/缓存live-replay一致性；先检查本地已有项、同步/指针生命周期与正确画面后A/B，再考虑GPU线程拆分。按页取消flush有stale GPU-written code风险，不能盲移植。
- 新确定性生产缺陷：AccelerateHostConditionalRendering的gen_lookup在仅+4命中时解引用旧end；现赋it_current=it_current_2，ObtainQuery nullptr保持cpu_addr/found_query=null且不dirty。未改condition基址/主项优先级/跨页规则，未改变shader语义。
- 永久 tools/test_query_lookup.py 直接提取实际生产lambda编译，MSVC checked iterators、每案子进程。baseline14案中3个邻项end故障3221226505+1个null故障3221225477均确定性复现，modified14/14 exit0；fake cache/query/address translator明确不是完整QueryCache/Vulkan/guest回写闭环，更不是Kirby黑屏根因证明。
- 5步Release实际生产query编译/库/DLL/EXE重链成功；匹配10EXE/DLL/PDB归档与部署hash核验，未启动游戏/未采本轮loaded modules，不称live验收。此前SR30/31/CSM拒绝和黑屏仍开放。
- 下一P0：复用正常初始化GPU/MemoryManager/BufferCache/QueryCache/Scheduler的无游戏真实后端台架，专属channel/report页，Enable→draw→Report→fence→guestwrite→Reset→分pass→conditional consumer像素；裸ctypes host query不能替代。台架成本高于一个query cpp独立链接，先通生产初始化/资源生命周期；必要时32事件consumer窗口，保持默认off/有界。
- 窄修四件及独立source+code binary copy rollback见nx2-query-lookup-20261103。当前生产修复/部署保留；rollback仅在标记的副本上测试，恢复原hash与原4个确定性反例，主线不回退。未Git提交/推送，不发送游戏输入、不修改ROM/keys/固件/存档/驱动。


## 2026-11-03：guest写回到真实CPU条件消费者台架，快更新工具入主线
- 新tools/query_guest_fixture.h、query_guest_harness.cpp、test_query_guest.py合法16KiB IMemory/IDeviceMemory宿主，直接执行完整GuestStreamer模板、真实MaxwellDeviceMemoryManager注册/映射。CPU SyncValues适配器明确不是Vulkan QueryCacheRuntime；pending不提前写、32/64宽度/邻字、invalidated/rewritten过滤、slot复用/重复sync/零最大值通过；不生成timestamp。
- 新tools/query_condition_harness.cpp、test_query_condition.py以真实MemoryManager(device-memory overload)和真实Maxwell3D构造，公开CallMethod触发生产ProcessQueryCondition/ShouldExecute；无private绕过、不伪造GPU引用。实际生产video_settings.cpp+video_enum_strings.cpp参与链接，Rasterizer只做CPU侧映射/flush观察，其余未预期操作抛错。
- 新鲜实际Vulkan native-SPIRV GPU查询256/0/224→输入报告hash→实际GuestStreamer写回→实际GPU内存映射→实际CPU条件31cases全部通过：pending/sync、Conditional双字段、Equal/NotEqual、Always/Never、24B Compare跨4KiB页及保留字段。它是独立GPU数值来源后CPU适配器导入，尚未执行生产SamplesStreamer/QueryCacheBase/hostconditional消费draw，不称完整生产ZPass闭环。
- 负例单独复制实际maxwell_3d.cpp，将Conditional &&改||；实际编译该生产函数副本后台架以HARNESS_ERROR actual CPU condition mismatch/exit1检出，外层expected-negative exit0。主源码不改。早期探针include/private访问/link/初始化回调失败保留，不称游戏崩溃。
- 新永久tools/refresh_ns2_references.py：显式调用，默认无后台网络；--out必须新目录，--previous留旧pin；核分支HEAD+release tag实际commit，--sources只抓变化HEAD的4个目标小文件，2MiB响应/12s请求、Forgejo20s限时。5自测（未变/变化/缺失/坏SHA/无旧pin与build数字非顺序）通过，5HEAD新鲜核验均未变，成功下载hash复查。Takoroka仅HEAD、不冒称最新源码审查。
- reference/CONTRACT.md：公开NVIDIA专利明确TR→batch各primitive在视锥/用户plane外，但原CSMTEST.A.FCSM_TR与该TR合同的ISA连接、Sa/batch/hidden state未认证；一般DCE反证仍成立。此轮软件消费者通过不许可CSM恒false。
- 下一P0：在实际初始化Vulkan backend中执行SamplesStreamer+完整QueryCache CounterEnable/Draw/Report/fence/guestwrite/Reset/分pass与hostconditional消费像素；现台架先固定已通过CPU对照，避免重新猜比较结构。同时保留SR30/31/CSM语义合同任务，无新契约不扩大泛搜/全量日志。
- 本轮只新增6工具与更新4docs，无生产源码/二进制改变、无游戏启动/输入/ROM/key/固件/存档/驱动操作；之前query窄修匹配10部署hash保持。baseline既有lookup14/modified实际CPU31/负例/独立副本source rollback与四件见nx2-query-contract-20261103。Kirby黑屏仍未修复，未Git提交/推送。


## 2026-11-03：真实 SamplesStreamer GPU 台架、查询写回宽度与设备特性窄修
- 已定位通用生产缺陷：无时间戳报告在QueryCacheBase按32位注册/CPU写回，但SamplesStreamer::PresyncWrites的HostSyncValues.size固定8，真实GPU复制覆盖相邻0xa5a5a5a5为0。现HasTimestamp保留8，否则sizeof(u32)=4；不改计数格式/查询偏移/完整时间戳布局。不把此缺陷当作Kirby黑屏根因证明。
- 新显式CreateSamplesBackendLab、Scheduler::RequestLabRenderpass及外部samples_backend_harness.cpp/test_samples_backend.py：合法实际Device/Scheduler/MemoryAllocator/DescriptorPool、真实VkQueryPool→结果复制→生产prefix scan→GPU下载→真实MaxwellDeviceMemoryManager映射；真实fence后Pop/有界CPU回调，16x16独立GLSL一次/两pass256/512、Reset0、无时间戳邻字保留。非原Kirby shader，不是完整QueryCacheRuntime/BufferCache。
- Khronos校验新增实际debug messenger，消息上限32且每条512B，设备资源销毁后验错；错误不隐藏。Headless instance缺VK_KHR_surface属实验初始化，现使用Windows instance扩展，不创建窗口/swapchain。生产Device启用了不兼容robustBufferAccess+buffer UpdateAfterBind，规范VUID-VkDeviceCreateInfo-robustBufferAccess-10247；property false且core robust true时只关闭四buffer UAB，保留robust、sampler/image UAB与本地MoltenVK特性。
- 真实生产分支8组合test_device_feature_filter.py通过；0/A(width)/B(device)/A+B同GLSL/adapter实际GPU矩阵：0和A准确检出设备validation1，0和B准确检出旧邻字覆盖；A+B邻字保留且validation errors0/warnings0/loader_errors0。expected-negative外层pass不是原缺陷消失。失败编译/validation日志保留，不当游戏崩溃。
- 新query_cache_fixture.h/query_cache_harness.cpp/test_query_cache.py合法实际Host1x/GPU/MemoryManager/ChannelState、完整生产QueryCacheBase模板17cases，controlled GuestStreamer/CPU SyncValues/OS timestamp；不是SamplesStreamer/Vulkan。初始化GPU不必然要求完整OS服务；裸未构造引用路线弃用。此前actual CPU condition31及lookup14在新生产库后复测通过。CPU回调不冒称GPUfence。
- 永久工具外部显式调用，无默认监听/游戏输入/后台网络/每帧采集；lab入口默认不调用。生产SyncWrites增加一次runtime/lab分支，未实测性能开销，不宣称零生产开销。编译180s/GPU worker60s界限；独立目录、源码/库hash前后核验、异常先drain后销毁。
- 最终Release7步成功并冻结4生产源，匹配10EXE/DLL/PDB归档与部署repo/bin/x64/Release；本轮未启动游戏/未验live loaded模块，不称整游戏验收。快更新5参考HEAD再次核验均未变，最新latest-heads-final；Takoroka仅HEAD，不称源码已抓取。未Git提交/推送。
- 下一P0：把actual SamplesStreamer与actual QueryCacheBase在同一合法runtime/guest memory连接，接通生产QueryCacheRuntime::SyncValues/BufferCache/hostconditional消费draw像素；覆盖Reset/分pass/fence/取消重载。当前CPU模板+GPU streamer是两条局部实测链，不称完整联合闭环。SR30/31/CSMTEST.A.FCSM_TR与真实batch/clip hidden state合同仍开放，禁止constant/false/skip放行；正确像素优先于FPS，跨NS1/Pokopia/Kirby及长期性能待验。
- 另列待办：Qualcomm禁shaderInt64修改features.features副本但实际core node为features2.features，本轮AMD窄修不顺带修改。证据nx2-samples-backend-20261103四件、原8/新7、独立source+lab-executable-copy回滚；主线15文件和新部署保持，非live emulator回滚。无ROM/keys/固件/存档/驱动修改。


## 2026-11-04：真实 QueryCacheRuntime／BufferCache／GPU 条件消费者联合台架与零比较窄修
- 新tools/query_gpu_harness.cpp、query_gpu_fixture.h、test_query_gpu.py直接构造合法Host1x/GPU/channel、真实Device/Scheduler/Staging/BufferCacheRuntime/BufferCache/QueryCacheRuntime/QueryCache；没有lab SyncValues替代回调或裸未构造引用。真实生产SamplesStreamer→prefix scan→QueryCacheRuntime::SyncValues→BufferCache→hostconditional消费draw→像素读回→GPU fence/Pop→CPU完成回调→实际Maxwell3D::CallMethod条件判断在同一guest mapping上运行。
- 原零比较快捷路径只看低u32，把(0,1)判为(0,0)。现IfEqual/IfNotEqual的is_null优化使用8B FullSynchronize+compute全两字Zero8，反相flags分别正确；Equal24原两字段比较保留。Resolve新增{compare_to_zero,word_offset} pushconstants、descriptor按设备alignment向下对齐、shader uint words[]只读取对应8B或24B字段；非u32对齐零shortcut走CPU回退。此处Zero8是全零相等，不照抄Eden含糊AND分支。
- 联合诊断还证实Device仅加载extension但未启VkPhysicalDeviceConditionalRenderingFeaturesEXT，compute barrier报VUID04092。现feature宏加入查询/启用链、移除同名misc项、getter同时检查extension与feature，feature不足清零节点并移除扩展；unsupported不创建HCR resolver/buffer。recipe C++ payload映射生成30types，新pipeline_probe whitelist1000081001:2，test_conditional_feature_recipe.py实际ABI parser4组合+12坏输入通过；parser不是GPU oracle，unsupported真实设备尚未运行。
- 最终LOAD而非DONT_CARE保留显式黑清屏。旧query路由与修复路由同一当前合法feature-enabled Device/currentcompute实验：19cases，其中14 Equality的旧4像素偏差→新0，含8组零pair、4byte非descriptoraligned地址、页末0xffc/0x1ffc和4个Equal24真实GPU写current字段/BC实际下载。REAL_CONDITIONAL_FEATURE_ENABLED value1、完整资源销毁后Khronos errors0/warnings0/loader0。基线oldqueryCPP+oldquery.h overlay，不是整套旧部署；早期DONT_CARE/mixed build/missingfeature报告保留但superseded，不当验收。
- 仍开放Conditional普通模式：本地CPU AND、现GPU low32、独立Ryujinx snapshot ulong非零OR存在三方分歧，四场景实测保留1项本地CPU/GPU偏差；本轮不选未经认证硬件合同。当前最新Eglantine/ssky六文件虽有AND修复，但同源fork不是硬件证明，NVIDIA class header仅有枚举。SR30/31/CSMTEST.A.FCSM_TR真实batch/hidden-state合同也仍开放。
- 新独立P0反例：无producer零报告publicquery value0/flags18/pending_sync0，客体CPU退役后(0,0)，真实hostconsumer仍绘制256像素，而AND/OR均应false。当前是已复现错误，缺“实际GPU predicate buffer内容→消费draw”的最后因果采样，未将旧seed推断当根因。下一优先有界8B BC GPU读回/同步事件，确认no-current-query路径是否缺GPU发布，再做通用零报告同步修复；不能跳predicate/硬解锁。
- Windows Release统一新Device布局42步及最终5步重链通过，7生产输入冻结，新匹配10EXE/DLL/PDB归档并更新repo/bin/x64/Release；cache17/CPUcondition31/lookup14/真实SamplesStreamerGPU回归通过。5参考HEAD再次核最新并保留旧pin/UTC/hash，均未变；Takoroka仅HEAD，Ryujinx为本地snapshot，NXbox当前source请求失败仅待核线索。未启动整游戏/未测live加载模块/无FPS提升或Kirby画面修通声称；未Git提交/推送。
- 永久台架外部显式运行，默认无listener/后台网络/每帧GPU读回，不发送游戏输入。资源异常先GPU drain再销毁，编译180s、worker60s，validation消息32条/512B；生产增加Zero8 compute成本尚未性能A/B。工具保留与语义修复有效分开，不以工具通过替代整游戏。
- 证据nx2-query-gpu-loop-20261103（跨午夜继续使用开始时目录名）：原12/新4/主线16文件保留，四件与独立source+lab executable copy回滚，回滚后的原query代码重新编译在同一合法Device上复现Equality4，原parser恢复missing4。正式部署不回退，原10binary archive指针/hash保留；整旧模拟器行为未运行回滚。跨NS1/Pokopia/Kirby、停止/重载/长稳/性能及原native shader仍待验。


## 2026-11-04：Reset-origin 零报告 GPU 发布与真实因果反例
- 已证实旧 SamplesStreamer 在 Reset 后无 host query 的 final zero 只标 FinalValueSynced、不入 GPU 同步：公开 query value0/flags18/pending_sync0；HCR 后真实 BufferCache NoSynchronize 返回 region 的 8B GPU 读回仍 (0xa5a5a5a5,0)，consumer 错绘256像素，CPU completion 最后却为(0,0)。采样插入8B copy+Finish但不上传CPU/不drain回调；旧on/off同错绘256，排除该采样单独制造错误。不是读取私有hcr_setup，也不是Kirby唯一根因证明。
- 通用窄修仅在 !current_query && empty_since_reset 确知 Reset-origin zero 时排 pending_final_zeros；Reset置true、ReserveHostQuery置false（包括Draw0）。HasPendingSync覆盖零队列，SyncWrites即使普通stash空也发布；独立immutable 8B DeviceLocal buffer懒建Fill0并TRANSFER_WRITE→READ，实际 QueryCacheRuntime::SyncValues/BufferCache复制4或8B。过滤invalidated/rewritten/orphan，Free删除ID防复用，不把零报告塞进会Close空current_bank的flush队列，不改CPU完成语义，不把历史accumulation当零源。
- mutex顺序Free的base guard→zero guard；Sync仅在zero guard内收集metadata，释放后allocator/runtime/BC调用。默认不新增每帧读回/监听；生产多一零队列检查及首次buffer/按需copy，未做性能A/B，不称零开销。
- 三组最终同20case actual QueryCacheRuntime/BufferCache/SamplesStreamer/GPU/guest/CPU联合台架通过：旧cpp基线GPU旧sentinel/consumer256；修复capture与off均consumer0，capture GPU(0,0)，CPU pending仍sentinel直到退役。14 Equality偏差0；Conditional普通模式仍保留1项CPU AND/GPU low32分歧，full_semantics_acceptance=False。每组Khronos errors0/warnings0/loader0。
- 真实QueryCache completion历史Draw256→Report/WFI/retire，再无Reset/Draw的bankless报告仍256且不入zero队列；历史非空epoch bankless的GPU发布尚未覆盖，禁止概括为所有bankless修复。新samples_zero_backend_harness/test工具14cases覆盖构造/Reset4/8宽度邻字、三flag、Free同ID复用、重复sync、Reset旧pending、zero+Draw混合、256/512真实GPU与实际streamer amend256。后者非完整QueryCacheRuntime完成链，完整历史链另由20case证明。
- root复跑cache17/actualCPUcondition31/lookup14/原SamplesGPU与新zero14全部通过；CPU31导入历史gpu-final samples256/0/224而非本轮新GPU来源。新工具首轮history误设accum导致512已移除错误编排，失败证据保留，生产未为适配工具改语义。新工具基线缺本地header失败是编译布局，不是GPU/游戏故障。
- Release冻结source成功，匹配10 EXE/DLL/PDB已归档部署repo/bin/x64/Release；未启动游戏或核live loaded模块。5参考HEAD已刷新保留旧pin/UTC/hash，分支HEAD未变；release tag单独记录，不以build数字推ref顺序，Takoroka仍仅HEAD。未Git提交/推送。
- 证据nx2-query-zero-publish-20261104：原8/新2/改10，四件与独立sparse source+lab executable回滚；副本恢复原cpp重新编译真实联合台架复现旧sentinel/consumer256，主线修复/10部署保留，非整旧模拟器运行回滚。不发游戏输入、不修改ROM/keys/固件/存档/驱动。
- 下一P0：历史非空bankless GPU发布的真实最小反例/取消重载生命周期；普通Conditional硬件合同仍需独立证据，不选择猜测AND/OR。Kirby目标原shader SR30/31及CSMTEST.A.FCSM_TR batch/hidden-state仍阻断，继续规格/独立实现/真实数值合同与实际Lower→GPU实验，禁止constant/false/skip放行。跨NS1/Pokopia/Kirby、长稳/停止重载/画面/性能仍待验，FPS不当可玩验收。


## 2026-11-04：历史bankless与跨WFI累计的实际GPU反例（工具推进，非生产修复）
- tools/query_gpu_harness.cpp/test_query_gpu.py从20扩展25case；新增4个历史bankless（256/512×前producer未退役/已退役）以及2个无Reset跨WFI Draw256+Draw256的实际Equal24消费者。合法实际QueryCacheRuntime/BufferCache/SamplesStreamer/GPU/guest/CPU回调，无注入累计值，不发游戏输入。
- 已确定缺陷A：4历史case pending_sync0，GPU predicate(0,1)，guest完成256/512且CPU_should_execute1，但consumer0；无新Draw非零报告确实缺GPU发布。已确定缺陷B：2跨WFIcase GPU(256,1)，guest完成(512,1)，独立GPU写current比较字段(512,1)，真实Equal24 consumer0，而CPU相等判true。普通Conditional AND/OR歧义不参与B的数值oracle。
- 最终capture25/off25同6反例，原14Equality偏差0/Reset-origin零报告consumer0保持；Khronos errors0/warnings0/loader0。expected-history-defect与expected-accumulation-defect必须显式开启，expected-negative PASS仅表示工具捕获已知缺陷，生产缺陷仍存在。默认要求无缺陷，应exit1；分别通过严格history/accumulation负对照检出。
- 捕获复用实际BC NoSynchronize region8B，不CPU上传/不drain回调，但增加copy+Finish；off对照保留。新B初版未GPU写current导致走既有both-QC/CPU EndHCR分支、画256，不是计数正确；失败final-capture保留，最终ClearBuffer真实写current再测，不把错误探针结果隐藏或改生产以适配工具。
- 只读审计：单slotPresync会Fill accumulation_buffer0，CPU历史值来自真实Replicate+amend回调；该buffer不是可直接认证的历史源。仅复制mutable最后值也遗漏跨WFI累计、Report→Reset→Draw的版本及GPU寿命。prefix两版max_accumulation_base-1在0时的潜在索引下溢仅静态候选，未动态认证；多dispatch索引也待验，不冒称根因。
- cache17与实际SamplesZeroGPU14新鲜回归通过。5参考HEAD刷新核验，保留旧pin/UTC/hash；生产cpp和此前匹配10 EXE/DLL/PDB均保持，未新build/deploy/game/live-module/FPS验收，未Git提交/推送。Kirby SR30/31/CSM障碍仍开放，这些人工GLSL反例不证明原游戏因果已修通。
- 证据nx2-query-history-publish-20261104：保全原7（含未改生产cpp），修改6（两工具+四docs）、无新主线文件；四件、独立copy原7及旧20case lab executable回滚，恢复旧harness/runner后实际重编译20case。主线25case工具/既有生产修复/匹配10部署保留，非旧模拟器运行回滚。

- 下一P0生产前置：实际GPU epoch总和模型覆盖首次单slot/多pass/跨WFI单slot/批次中Reset；先补prefix真实accumulator读回与两实现/多dispatch测试，定位首次分歧。
- 随后做报告时不可变8B版本；旧Report256→Reset→Draw512→WFI须旧256/新512，旧Report256→无ResetDraw256须旧256/新512。guest发布经过flags/ID取消过滤，4/8B邻字保护，源buffer保留到GPU提交tick完成；零队列不塞空bank flush。
- 合并前矩阵：0/A历史发布/B累计修复/A+B；每项局部数值不变量、联合消费者像素、取消/重载/跨游戏回归。原游戏SR30/31/CSM独立语义合同继续，禁止猜测常量/false/skip放行；正确性通过后再性能A/B。


## 2026-11-04：跨WFI GPU累计通用窄修、真实prefix台架与执行参数定位
- 修复SamplesStreamer单slotPresync原Fill0丢掉GPU carry：现在用当前query实际bank→packed offset的最后8B复制到accumulation_buffer，带GPU读写barrier，保留下一WFI累计；不能用num_slots_used-1，因为计数包含Reset丢弃或未Report槽。当前内部query加入resolve集合但保持orphan、不写客体；无current epoch（旧Report后Reset）保持zero/false，避免旧carry重新注入。
- 第二核心根因：last_accumulation_checkpoint0/numslots2被std::max转为实际Run max2，shader以512减去最后output512令accumulator0；现std::min保留Reset位置0。外部实际GPU探针曾读raw256/256、accum0，字段trace不等于执行push值。两prefix shader新增max0无前缀guard；midReset扣除前prefix中含旧carry的部分，避免最终accumulator -2而非8。不选择未经认证Conditional AND/OR，不涉及原游戏shader放行。
- 最终同28case actual QueryCacheRuntime/BufferCache/SamplesStreamer/GPU/guest/CPU联合台架 capture/off均通过：两跨WFI原GPU256/CPU512、consumer0→GPU512/CPU512、consumer256；3新增审计边界分别512（丢弃旧槽Reset）、256（pending旧report后Reset）、768（report后有新未Report Draw），GPU/CPU及Equal24 consumer256一致。原14Equality偏差0/Reset-origin zero消费者0保持。历史bankless4仍明确expected-negative、GPU0/CPU256或512；普通Conditional保留1分歧，full_semantics_acceptance=False。
- 新tools/prefix_backend_harness.cpp/test_prefix_backend.py直接执行实际QueriesPrefixScanPass::Run/descriptor/dispatch/barrier，受控u64输入与真实GPU prefix/accumulator/物理guard读回。独立C++副本只选择真实subgroup/fallback构造器shader、增加test-only bounded32实际PushConstants结构采样；GLSL→SPIRV明确hash，不以CPU oracle替代GPU。新增实际normalized min/max/count/offset，源库/header/多个EXE的SPV字节一致已审计，不将请求字段误当实际执行字段。
- 两variant small4：Reset0、carry256、u32carry、midReset全部0偏差/validation0；旧midReset -2→新8。forced subgroup all8无Reset2048/2049/4097正确，但Reset2048边界仍1已知偏差（accum2066应1）；forced fallback all8仍4大数组/dispatch偏差（1025起前缀错），不是全prefix修通。各expected-negative PASS只代表诊断检出，完整Samples跨batch Reset报告映射/大fallback仍待修。
- 新鲜cache17/actualCPUcondition31/lookup14/SamplesZeroGPU14回归通过；CPU31导入历史GPU样本而非本轮新游戏样本。生产3源冻结Release构建成功，匹配10 EXE/DLL/PDB归档部署bin/x64/Release。5参考HEAD重新核验保留旧pin/UTC/hash，分支未变；当前Eglantine pin c91b9a71，Takoroka仅HEAD。未启动整游戏/核live modules/测FPS/性能，无输入/ROM/keys/fw/存档/驱动操作，未Git提交/推送。
- 初版rawlast/无条件carry在源码审计发现两个边界，保留firstbuild/modified25证据后修正；28case第三边界曾暴露实际accum0，保留失败final-capture和两个外部trace副本及GPU回读，定位max归一化而非混shader。外部临时probe不进生产默认路径，新工具正式入唯一repo；生产有额外copy/orphan范围与prefix工作，开销尚未A/B。
- 证据nx2-query-accumulation-fix-20261104：原9/新2/主线改11，四件与独立copy source+lab rollback；恢复原cpp/两shader/旧25harness+runner，原源码和GLSL重新编译在同一合法当前Device/libs复现GPU256/CPU512；恢复两shader再做实际prefix复现旧midReset。主线11及10部署保留，非整旧模拟器运行回滚。
- 下一P0：基于已认证GPU carry做报告时不可变版本的历史bankless发布（Report256→Reset→Draw512须旧256/新512），flags过滤/4或8邻字/Free-ID复用/GPUtick寿命；0/A历史发布/B当前累计/A+B矩阵尚未执行。本轮B通过，不把A未过回退B。随后prefix Reset跨dispatch与fallback扫描树、packed Reset映射、取消/重载；Kirby SR30/31/CSM batch/hidden-state合同与正确画面、跨游戏/长稳/性能仍开放。
