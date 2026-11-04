# Kirby 黑屏续诊（2026-11-04）

## 版本与验收边界
参考固定在 Eglantine nx2 `09e539a65ccf8d16a2dc61ee37db0ba4ab20db69`；不是本机旧 build178 EXE 的功能验收。
唯一源码 repo；正式游戏操作由用户验收。本轮自动测试没有游戏输入，没有 ROM/keys/固件/存档/驱动修改。

## 已完成的首项窄修
LEA 低字 unsigned carry-out 与捕获 uniform UP0/UP1：实际 Lower→CFG→TranslateProgram→SSA→SPIRV→Vulkan 64 数值/谓词用例通过；独立 root 复测通过。原 60 用例基线与独立源码回滚通过。
移位产生的丢弃位不直接算加法carry；实际 IADD3.X 消费carry；P/UP/源寄存器保留。
证据：`D:\project\nxemu-switch2-upgrade\nx2-kirby-lea-fix-20261104`。

## 首轮实际游戏验证
证据：`D:\project\nxemu-switch2-upgrade\nx2-gsv-reply-probe-20261104\final-live`。
60秒、103采样、正常退出0、无forced cleanup、502 Composite；5个实际加载EXE/DLL的hash匹配10件归档部署。
两次1280×720 GPU offscreen捕获均black_ratio=1；黑屏仍开放，不是UI遮挡/FPS计数替代像素。
剩余native拒绝：HMMA528、special-register4224、新0xc11六次；既有LEA carry与local spill拒绝类别已消失。
末样本24.8179 FPS/40.5214ms仅为本轮诊断采样，不是正确画面或性能优化验收。

## 新的确定性工具与ABI证据
现有guest trace仅保存内部cmd_buf，32条IPC会被HID刷新；保留模块生命期gsv115首8次，超额计数，无后台监听。
发布hook在actual command-buffer WriteBlock成功后，记录准确write_size，并仅对已保留token读取TLS首48B；仍默认debug-detail关闭。失败/未采集不冒充已发布。
启用：已有SDK launch `--debug-detail 1`；输出 `bridge/diagnostics/guest-activity.json` 中 `gsv115_query_first`。
额外8槽、48B×8有界读；一般JSON字段增加可影响recent遗漏，现有collector_ns/omitted仍保留；未声称零成本。
CPU actual-header测试验证默认off、首8/超额4、500次HID覆盖、3字发布长度、未写尾字与不可读状态、重复发布不覆盖。

actual ROM SDK导出 BootNativeMemoryManagement(MemoryBlockModeBits) 位于0x40ff20/0xd0：读取result@TLS+8，再读取u64@TLS+12，保存sdk+e9e020。
本轮gsv115真实发布仅12B，TLS尾保留请求，SDK读到0x001501d900000000；完整20B ABI要求确定。
Switchbrew将115命名NativeMemory/BootNativeMemoryManagement；上游QueryGrantedFeatures名字不当作规格。
返回哪些mode仍未认证；普通CreateNativeMemory通过bit1控制svc0x80/GpuAccessible/114，而本轮已知主路径111(mode3)+423不走该helper。因此ABI缺陷与黑屏显示因果必须分开。
独立ROM解压/dynsym/capstone证据与原网页/hash保存于 `nx2-gsv-reply-probe-20261104`。

## 下一语义任务
- 新0xc11是ordinary R + uniform UR LEA，捕获R6=u32((R4<<6)+UR4)，同时补uniform-bank识别保护；必须实际GPU测试，不能只加switch。
- HMMA捕获形状K16合法，当前先被tensor入口拒绝。最新Eglantine FFMA/f32每8乘积半舍入，本地HFMA2逐乘积舍入；直接打开环境变量不等于正确修复。
- 先单warp32 lane非对称输入、C/别名/临时寄存器、舍入反例，再4warp；实际GPU oracle与guest-warp映射共同验收。完整shader输入/绑定/同步和输出消费者仍待补。
- SR30/31与CSMTEST仍缺独立数值/隐藏状态合同；不以Eglantine常量/恒false换取放行。
- gsv115完整响应的mode契约及真正消费者轨迹独立验证；不为全grant/空成功迁就游戏。

## 第二项窄修与联合部署
0xc11 无HI/X/carry的R+UR低字形式已补，同时补uniform-bank扫描；支持正/负UR和shift0/6/31，保持P/UP/全28URbank及R215边界。
在最新冻结源码上，root独立真实GPU回归139（carry64+uniform75）通过；39负例、9边界接纳；独立副本回滚64及local-spill24+CFG4通过，SPIRV/Khronos均0。
最新header SHA256 `2eaccaa52fbf526160d4e78067bc45b4caa5828dd3f360da2310970c14774f05`；永久tool SHA256 `3db300af5aa9f666ea1a09041c2f70502d09a858c5e31dc0ec60a1ad18845848`。
统一Release target nxemu增量5步成功；部署10件，第二60秒测试正常exit0、426 Composite；真实加载5hash匹配。新0xc11与carry/localspill类别均消失，剩HMMA445、special-register3316。
第二运行真实GPU帧仍black_ratio=1；没有正确画面/可玩声称。较晚一次capture请求误在正常退出后发出，exit4原始结果保留；不得把失败截图请求当黑像素或游戏崩溃。
证据 `nx2-kirby-lea-uniform-src-20261104` 与 `nx2-gsv-reply-probe-20261104/uniform-final-live`。
下一优先不是更多盲patch：补HMMA实际完整shader/输入与guestwarp32实验，SR30/31数值合同；gsv115返回mode合同独立处理。
