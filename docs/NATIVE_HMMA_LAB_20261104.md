# HMMA / F16 数值实验台（2026-11-04）

## 已取证输入
现有 --shader-fixtures=1 已能保存完整原生程序，无需另建原始shader捕获器。
Kirby捕获1979指令/31664B，SHA256 02559a11135a4bbf88d98f04850387f42eb6359bd57c7711cd540c1ce01e1cf3；捕获结束complete不是全GPU资源重放。
失败offset0x14d0：0830723c 0000000c 000008ff 041ff000（HMMA K16）。
新增fixture元数据来自已有环境/QMD/TLS同program dispatch，未新增resource读取或GPU同步。
真实环境：workgroup128×1×1、grid64×24×1、shared6912B、local0、texture-cbuf0、cbuf0 enabled/256B。
7件shader原字节完整性已核验；cbuf/texture/SSBO真实输入与GPU可见性仍缺，replay_ready=false。
默认Lower拒绝；仅独立probe进程tensor1完整Lower通过68054 Maxwell指令/1979 source offsets；不是IR/SPIRV/GPU验收或正式游戏开关。

## 真实后端数值台架
唯一repo/tools/test_native_hmma.py + native_hmma_harness.cpp，调用真实Lower→CFG/TranslateProgram→SSA→SPIRV→Vulkan；必须显式 --tensor-emulate。
固定adapter candidate0不冒称原游戏设备身份；本次native subgroup64/min64/max64，requiredSubgroupSizeStages0。
严格实际32前置路线正确停止，随后显式 --host64-partition 使用生产支持的宿主64/客体逻辑32路径。
真实shaderFloat16支持已查询并启用；Profile/HostTranslateInfo、vote/shuffle能力匹配设备。
六矩阵包含非对称单脉冲/整数/C非零/舍入反例与128线程四logical-warps不同输入；实际SHFL31不串warp。
固定profile与同RTE16执行模式：local baseline6、reference FFMA modified6、独立rollback6均通过；SPIRV-val/Khronos errors/warnings0。
GPU读取D、lane/SHFL与R200..203保存寄存器；未读出A/B/C各来源寄存器，不声称全部source preservation。
前置能力、错误profile、越界工具哨兵、默认half转换模式失败均保留；失败不是游戏崩溃。

## 与参考的真实区别
参考 Eglantine nx2 HEAD09e539a65ccf8d16a2dc61ee37db0ba4ab20db69；GSV仍与旧pin相同，未得到全mode合同。
当前local逐项HFMA2半精度累加；最新参考FFMA/f32每八项舍入到f16，且入口另受DLSS健康/warp32条件控制。
A全部1、B第一项1其余2^-12时：同RTE配置local=3c003c00、reference=3c043c04，双方匹配各自模型。
参考copy在实验中用明确受控寄存器保存替代bank2保存机制，细节/hash记录在nx2-hmma-numeric-20261104；不是生产原样移植。
两个模型都通过人工输入不认证console完整累加/FTZ/NaN合同；尚无独立NS2硬件样本。

## 首次数值分歧
actual native F2F.RN 00307304 00000008 00200800 00000000的IR保留RN，但旧SPIRV EmitConvertF16F32仅OpFConvert，忽略该控制。
同adapter/float16/profile，默认convert的halfway输入输出3c01（RTZ），RTE16对照输出3c02（RN ties-even）。
真实RTE16/RTZ16 property均1，roundingModeIndependence0；test-only执行模式有效后32lane正负/halfway等输出匹配，val/Khronos0。
这不是HMMA矩阵布局失败，而是一个可独立重放的F32→F16舍入合同缺口。
逐指令RN+FmzNone整数转换窄修已完成独立overlay与主线认证；160输入RN偏差55→0，回滚恢复55。7排除路径224输入保持，SPIRV逐字一致，各12 GPU kernels/12 spirv-val/Khronos errors和warnings0。不全shader设置RTE，不扩大RZ/RM/RP/FTZ/DontCare；FP32 fallback未修改。

## 调试桥和后续
compute_environment的强类型stage/program/slot、正dispatch token、8bitmask、availability一致性、legacy路径已补24负例。
相同code不同QMD会去重为首个metadata；不当作逐dispatch记录或完整resource oracle。
下一优先：补cbuf/纹理/SSBO输入和同步→输出消费者链；tensor策略须显式上下文与缓存身份，不仅全局env。HMMA模型/入口与SR/CSM合同是不同任务，不用单项整游戏仍黑推翻已认证RN窄修。
SR30/31与CSM隐藏状态仍缺独立合同；不以常量/false放行。黑屏尚未验收，FPS不是正确像素。

## Root统一集成与实际游戏复测
- nx2-f16-rn-fix-20261104/root-independent-final独立重编译actual convert+dispatcher，12 GPU通过；root-hmma-modified-corrected显式reference lower-source六GPU通过，未改变生产HMMA。
- Windows target nxemu Release构建exit0；原10 EXE/DLL/PDB保存在binary-before-root，新10部署hash保存在DEPLOYMENT_ROOT.json。实际运行loaded5逐一匹配。
- 60秒无游戏输入，正常退出exit0、forced_cleanup=false；约40秒的显式GPU合成采集1280×720，921600黑像素/black_ratio1。该时刻黑屏仍在，不能外推所有后续帧。一次GPU Finish和同期独立GPU回归会影响时序，本轮不做性能A/B结论。
- 最新shader-rejections：HMMA K16@0x14d0拒绝514，special register4057；validated dispatch5037，submitted4523，not_submitted514。该证据确认矩阵任务未提交，但未证明其输出到当前黑合成像素的完整因果链。序列容量64，overwritten5021；reason统计不是完整逐事件轨迹。
- root-lea-uniform-regression实际GPU139通过（含mixed75）；root-spill-regression实际GPU24通过。未宣称跨NS1/Pokopia/玩法回归或全HMMA shader GPU重放。
- 四件与逐命令/输入/字面输出/exit、独立source rollback在nx2-f16-rn-fix-20261104和nx2-native-compute-metadata-20261104；工具/生产窄修保留。未Git提交/推送。
