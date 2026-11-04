# 空间回收与 HMMA 输入定位（2026-11-04）

## 已执行空间回收
- Everything先检索，再逐路径stat和SHA256；项目约47.322 GiB，唯一repo约5.202 GiB。实验二进制/构建产物占约31.14 GiB，frozen库逻辑约8.91 GiB。未用全盘递归遍历。
- 用户确认后执行84组/411文件的同内容不可变证据去重；325路径变为同卷硬链接，2路径因权限/属性不同跳过，其余为每组canonical。
- 实测释放20,594,368,512字节（约19.18 GiB），D余量39.37→58.55 GiB；本轮新版本归档/构建后复查约58.33 GiB。路径/文件名/SHA256保留；硬链接共享文件时间/权限，旧逐路径元数据在journal。
- 不处理唯一repo、当前build/bin、ROM、keys、固件、存档，不做整目录删除。Everything的1305个已消失临时路径在stat时排除，未作为删除候选。
- `nx2-space-audit-20261104`保留详细清单、325逐路径file-id差分、原hash/元数据、独立小副本去重/回滚实测及四件。完整19GiB生产拆链接回滚未执行；ROLLBACK.sh可按journal还原独立副本，需重新分配空间。
- 后续：冻结库/历史匹配二进制优先复用同SHA256不可变制品，禁止与会被linker/Copy-Item改写的活build/bin做硬链接；新out始终独立，旧证据不原地改写。共享冻结制品辅助模块和旧测试脚本接入仍待实施，不能把本轮去重当作已经修改所有freeze脚本。

## 新观测工具和接入
- `native_compute_input_fixture.h`仅在shader-fixtures开启、native-compute-inputs=1、明确16位目标FNV匹配时，在同program失败边界采cbuf0≤256B。
- 预留后读取，最多4次/module-load；同token重复不读，reload不退款，非法/未映射/异常/IO失败也消耗次数。原native bytes只引用旧fixture，不重新复制。
- reader为实际`MemoryManager::ReadBlockUnsafe`，不FlushRegion；只采CPU mirror，映射检查与读取非原子，GPU_visibility=false、replay_ready=false。未采纹理/SSBO字节。
- actual header13场景通过，root独立复验；launcher旧参数正例2/坏参数7，typed reader正例3/坏元数据23；新工具没有复制任何.lib。
- SDK启动参数：`--native-compute-inputs 1 --native-compute-input-hash 29e48ad9f9dee2e9 --shader-fixtures 1`；读取：`python -B tools/debug_sdk.py --bridge BRIDGE native-compute-inputs`。状态/时效与快照内容分开，不把退出后的快照当live状态。
- 源码root独立rollback恢复4原文件/移除3新增文件，collector-absent与旧launcher拒绝新参数均通过。正式主线保留新工具；不是整旧模拟器运行回滚。

## 实际游戏与输入差分
- Windows Release target nxemu构建成功，10部署hash核验，实际loaded5匹配；45秒无游戏输入运行正常退出0、无forced_cleanup、200 composites。
- detail0下未显式开启frame-capture，因此请求被正确拒绝，**本轮没有新像素采集**；不根据200 composites声称画面已恢复。前轮约40秒全黑结论仅适用于前轮采样。
- 实际捕获4×256B，token11/16/21/26，cbuf地址每次相差0xa00；run/generation/program匹配。全native31664B、1979指令、SHA256 `02559a11135a4bbf88d98f04850387f42eb6359bd57c7711cd540c1ce01e1cf3`已与目标FNV对齐。
- 前三输入SHA256 `bceb8aa6be49b10072fb866709f3508eaccb50573c9dda4178b7706dafe79cf5`；第四 `ecad4992d680eddb1f3a3fa188aded86a62a972275cb8ac75a16cd4268adc2c3`。仅两u32变化：

|cbuf字节偏移|前三|第四|实际shader角色|
|---|---|---|---|
|0x80|0x120028ad|0x12002bad|TLD/TEX所用纹理handle字|
|0x90|0x00003e0d|0x00003e6d|SUST所用surface handle字|

- 静态依据：native TLD b66共18、TEX b60共24，binding0且word offset0x20；SUST b9b共24、binding0/word offset0x24。Lower→Maxwell texture/surface translator把word offset乘4，texture_pass使用Environment::TextureBoundBuffer。因此角色有源代码/原指令依据，不是把任意u64猜成地址。
- 尚未采TIC/TSC、实际图像及版本，不能认定实际ImageId或像素改变。现有native_compute_submitted=true仅表示scheduler.Record排队，回调仍可能因HasBuildFailed return；不是vkCmdDispatch录制/执行/fence完成证明。

## 下一条固定路线
1. 按上述两个handle定位TIC/TSC和输出surface描述符，定点采格式/尺寸/地址；映射到实际ImageId/版本和失败shader角色，仍默认off、有界，不扩大cbuf全量读。
2. cbuf0x70/0x74经LEA形成global-read base候选，按真正LDG使用范围追；相邻0x78值不可直接当buffer size合同。
3. 同dispatch token分开queued/command_recorded/fence_retired，补输入资源→失败输出→消费者→合成像素链。
4. HMMA累加精度、SR/CSM合同仍单独认证；不全局tensor、恒值SR/CSM false、跳等待或D=C桩。已认证RN/LEA等修复不因单项整游戏仍黑而盲回退。

尚未Git提交/推送；正式游戏操作仍由用户测试。

## 固定下一采集recipe（只读静态认证，未执行descriptor读取）
- native0x08a0 TLD：`032e7b66 30002004 009e0730 000f4200`；0x3600 TEX：`44287b60 30002029 049e079d 000f4200`；0x4a40 SUST：`3a007b9b 6000242c 00100f00 0043ee00`。
- native0x1070 LEA：`01047a11 00001c00 078210ff 000fe200`读cbuf70；0x1090加进位：`ff057a10 00001d00 00ffe4ff 000fe200`读cbuf74；0x12f0 LDG：`04037381 00060000 001ea900 001ea800`以R4:R5为基址。没有独立LDC/ULDC，不能写成某条LDC捕获。24 LDG立即偏移0..2e00叠加动态R1，实际范围仍需运行值。
- texture.h:147-175中TexturePair只在via_header_index=false时拆TIC low20/TSC high12；vk_compute_pipeline.cpp:188-208实际依qmd.linked_tsc。必须先采linked_tsc与pool地址/limit，不能先假定false。
- false分支的预期候选：输入TIC28ad→2bad/TSC120；输出TIC3e0d→3e6d。下一只采上述TIC及必要TSC的原字节/布局/格式/尺寸/地址，记录CPU mirror或同步干预，再接实际ImageId/version。当前descriptor bytes尚缺，不猜实际ImageId/像素或声称原路径已执行。
