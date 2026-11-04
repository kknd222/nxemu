# NXEmu 与 Eglantine：Kirby 阶段差距对照（2026-11-04）

## 版本与证据边界
- NXEmu：唯一 repo 当前 local-spill 修复头 SHA256 `5f2b6c6028807572265c7fa42aa3438a23c08e4ec6fab08844ef70fd8215c873`；本轮比较没有修改生产源码或运行程序。
- 本机 Eglantine EXE 仍是 build178/ec667e25f9：SHA256 `3AA00D93B90E4577538BBEC22CDA8CDAB96265BB67342155B2EED21B1481A78D`。影片与 ClearSystemEvent PANIC 日志属于该程序。
- 本轮核验并抓取的 nx2 HEAD：`a666e4400b1a877145126411cce7ab49cf340793`；experimental HEAD：`1521742968665c171e19243331c5e2822f9d2420`。
- 最新发布 build189 的 tag 实际解析为 `7adcecc6d23b86d84e158572ea6ddd261fc1cd14`，不是上述两个 HEAD。发布版到 nx2 HEAD 的 lower/kepler/gsv/environment 四文件仍有816新增/118删除，不能将HEAD功能算作本机EXE或发布版实测。
- 参考裸库新ref与旧ref同时保留，没有在 NXEmu 上 pull/reset 或建第二套完整源码。

## 36组实际转换器对照
同一6个最小指令片段加 unconditional EXIT，分别调用两个实际 LowerToMaxwell，在default、EDEN_NX2_TENSOR=1、stub三种独立进程环境下运行。完整命令、输入SHA、原始输出及exit0保存于 `LOWER_COMPARISON.json`。
这些是转换器行为差分，不是完整游戏shader重放、IR/SPIRV/GPU数值或性能验收。SR31是由捕获SR30修改寄存器字段的最小用例，其余输入来自此前记录的字面指令；不把它们冒称全量ROM输入。

| 指令片段 | NXEmu default | Eglantine HEAD default | 解释 |
|---|---|---|---|
| SR30/31 | 拒绝 | 各转换为2条（含EXIT） | Eglantine后端仍是占位常量，转换通过不是正确语义 |
| HMMA | 拒绝 | 拒绝 | 在缺少tensor上下文的default片段中，两边相同 |
| LEA carry/high | 拒绝 | 转换为7条（含EXIT） | 可据整数合同建立下一项真实GPU窄实验 |
| STL128 RZ | 转换为5条 | 转换为5条 | 上轮补齐的128位清零形式已对齐 |
| LDL32 dynamic | 转换为2条 | 转换为2条 | 上轮动态读取形式已对齐 |

HMMA在tensor=1下，NXEmu输出215条，Eglantine231条；stub下均3条。条数不是性能或正确性评价。stub的D=C不是矩阵乘法，不作为修复选项。

## GPU／shader差距
1. **SR30/31**：本地native lower先拒绝，旧Maxwell路径返回float1.0位模式0x3f800000。新Eglantine放行并生成SR_WScaleFactorXY/Z IR，但SPIR-V `emit_spirv_context_get_set.cpp:589-597`仍明确STUBBED并返回0x00ff0000。两边常量不同，均缺独立硬件合同；不直接移植白名单。
2. **CSMTEST**：NXEmu未实现；Eglantine `lower_to_maxwell.h:1178-1188`固定false以避免提前EXIT。这是显式占位，不是批次/裁剪/隐藏状态通用实现。
3. **HMMA**：本地以HFMA2模拟，新Eglantine有f32 FFMA累加与half舍入路径。新 `kepler_compute.cpp:623-645`以DLSS链健康和blocker条件选择tensor入口；该链识别主要针对其他游戏，尚未证明适用于Kirby。补算术与补执行入口是两项耦合任务，不以环境变量开启或生成SPIRV当完成。
4. **Geometry**：本地native admission只接纳VertexB/Pixel；最新HEAD已接纳Geometry，并在 `shader_environment.cpp:462,584-587`传递geometry上下文，lower有OUT变换。宿主设备无geometry支持仍会跳pipeline；不能只删拒绝条件。
5. **LEA carry**：实际片段 `04047291 00000006 0f80103f 001fc800`，最新上游先做uniform映射再处理低字carry。需要测试移位、溢出、UP0保留及IADD3.X消费；完整数值合同仍待本项目GPU验证。
6. **失败缓存**：本地失败路径重新捕获/Lower/throw，限频日志不等于负结果缓存。Eglantine已有negative cache和失效删除，可减少重复失败成本，但必须按code内容、上下文和重载生命周期失效；不能缓存一个地址的失败直到永远。

## OS／HLE与当前运行阶段
- NXEmu最后60秒测试441次Composite、无PANIC；最终独立1280x720离屏采集black_ratio=1。本地内存类别拒绝消失，仍有SR/HMMA/LEA拒绝。
- 旧Eglantine日志已有1920x1080 VP9 frame600，停止影片后ClearSystemEvent非法状态触发PANIC。它到达更后的阶段，但不是Kirby正式游戏可玩基线。视频FPS不能与实时游戏FPS比较。
- **完整NX2视频服务缺口**：NXEmu根gsv无901/902/905/906/991/911/961等，也无INx2VideoSession；Eglantine有配置、真实FFmpeg线程、PTS、输出图像和syncpoint链。NXEmu已有NS1 NVDEC不等于NS2服务接通。OS→VIDEO模块需要显式Decode/Drain/输出与取消/销毁接口。当前NXEmu尚未到此阶段，不能将其确定为当前首个黑屏根因。
- **已调用的gsv协商缺口**：NXEmu日志命令4/11/101/115/201/301/401/402/434/1101/1102/1111/1501/1502落入auto-stub。其中115请求数据为w2=2,w3=0，NXEmu返回仅result成功；Eglantine QueryGrantedFeatures返回两字feature子集（上游全grant）。这是响应合同差异，尚未捕获客体实际消费与选路，不能全grant后宣称已实现所有特性。
- **事件**：ClearSystemEvent是SDK对象状态检查；不能把它等同NXEmu KEvent/ResetSignal缺实现。上游Stop/Finalize推进handed fence、100ms后继续写图像等策略需验证取消完成与数据完成，不直接复制。
- **ROM／固件**：两者已有Switch2 ACID布局、LooseRomFS和22.5.0相关记录，无新的重装固件根因证据。Eglantine数字update目录寻找可参考，但main大小匹配不等于BuildID/资源一致。
- **桥**：NXEmu已有run_id/generation、SDK控制、真实GPU离屏采集和数值实验台。需要补特定请求/响应与对象生命周期探针，而不是重建整个ADB式框架。

## 性能与日志差距（不冒称FPS提升）
- NXEmu该60秒诊断日志11,486,825B/64,665行，hid213未知包5892次；旧Eglantine日志1,168,231B/6825行。阶段/时长/诊断开关不同，不能用日志量直接证明速度差。
- Eglantine未知服务仅首次报告；可学习有界首次样本+累计次数+显式trace。本地仍重复完整包和fallback日志。上游无界static set不宜原样移植，要按run重置/容量上限/遗漏计数。
- a666的新UploadInChunks：对符合条件的>=64MiB二维图像按32MiB分段，用PlanUnswizzleImageRows/ExecuteUnswizzlePlan控制staging峰值；分段间Finish等待。它主要降低大上传内存峰值，不是消除PCIe传输或shader编译成本，可能增加同步等待。
- 纹理上传与上轮24.9MB稀疏回读是相反方向，不将该新功能说成进一步降低已测回读量。正确性先行后，再做固定场景/缓存/设备性能A/B。

## 下一轮优先队列
A. 同步取证gsv115请求、实际响应和客体消费者分支，解释早期协商差距；不全grant未知特性。
B. LEA exact片段：规格/独立整数模型→真实Lower/IR/SPIRV/GPU→UP/P保存与IADD3.X消费者→最窄生产修复。
C. 原生Geometry入口、Lower上下文、OUT/输出布局与宿主feature作为完整任务，不单改admission。
D. SR30/31与CSMTEST继续独立规格/数值合同；HMMA算术和DLSS执行入口分开验证再做联合矩阵。
E. 失败cache与有界服务计数作为观测/性能任务，不能替代上述语义修复。
F. 达到影片阶段再接完整NX2视频模块生命周期，并验证停止/重载/销毁；旧EglantinePANIC保留为负例。

本轮只读比较和独立小片段编译，没有修改NXEmu生产源码／部署、未输入游戏按键，未改ROM/keys/固件/存档/驱动。
