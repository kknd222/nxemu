# NxEmu Android 交接文档

更新时间：2026-09-01 12:30:00 +08:00
维护规则：后续每次完成阶段性开发、构建、安装、测试、定位或修复后，都要同步更新本文档，保证其他人可以随时接手。

---

## 0. 2026-09-01 官方 upstream 同步记录

- 官方 upstream `origin/master` 已 fetch 到 `138fb90`；用户 fork 主线 `kknd222/master` 已合并官方更新并推送，merge commit：`fa9c5e4 Merge official upstream into kknd222 master`。
- Android 分支当前：`android-nxemu-port-20260901`，在本地已合并 `kknd222-master-sync`（含官方最新），并保留本地 Android/UI/NCE 改动。
- Android 合并后的构建修复点：
  - `src/core/hle/result.h` 改为兼容 shim，统一引用 `src/nxemu-os/core/hle/result.h`，避免 Android 兼容 `src/core` 与 `nxemu-os` 两套 Result 定义重复。
  - 移除官方已删除 microprofile 后残留的 `MICROPROFILE_SCOPE`、`MicroProfileFlip()`、`EnterCPUProfile/ExitCPUProfile/EnterSVCProfile` 调用。
  - 恢复 Android NCE 诊断需要的 `NXLoaderSetting::Has39BitAddressSpace` 和 `is_39bit` 计算。
  - Android JNI 的 `Systemloader().LoadRom(...)` 已适配官方新签名：`LoadRom(path, 0, -1, ApplicationLaunchType::FrontendInitiated)`。
  - `system_display_service.h` 去重合并后的 SharedFrameBuffer 声明。
- 构建验证：`src/android` 下使用 portable Gradle/JDK 执行 `:app:assembleDebug` 已通过，产物：`src/android/app/build/outputs/apk/debug/app-debug.apk`，时间约 2026-09-01 01:40。
- 安装验证：已通过 ADB 安装到真机 `b72182d`，`adb install -r -d app-debug.apk` 返回 `Success`；启动 `MainActivity` 5 秒无 FATAL/闪退；随后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 待做：真机运行 Kirby/Metal Dogs 做 5 秒探针和按需延长测试；测试后记得 `am force-stop org.nxemu.app.debug` 并锁屏。

---


## 0.1 当前总体任务进度（2026-09-01 复核）

当前仓库/分支状态：

- `kknd222/master`：已同步官方 upstream，远端 HEAD 为 `fa9c5e4`。
- `kknd222/android-nxemu-port-20260901`：已同步官方 upstream + Android 分支改动；安装验证提交为 `33c0e7f`，本文档复核提交在其后。
- 最新 APK 已构建并安装到真机 `b72182d`；主界面 5 秒启动验证通过，无 FATAL/闪退；安装后已 force-stop 并锁屏。

当前功能状态：

- Android 工程/构建/安装：已形成可重复构建和 ADB 安装流程，当前约 **80%**。
- 主界面/外壳：已有游戏扫描、最近游戏、驱动入口、设置入口、每游戏配置入口，当前约 **65%**；但距离 Eden 完整 Material/封面墙/二级设置体验仍有差距。
- 运行页：已有 Surface、触控 overlay、HUD、日志、暂停/返回保护、自动日志，当前约 **65%**；仍需进一步 Eden 化、减少调试按钮感、支持更好的手柄/触控布局调整。
- `.dnsp/.dxci` 游戏加载链路：当前约 **60%**；Metal Dogs、Kirby、Mystic Gate 均已有进入画面的记录，但不同游戏仍需要分游戏配置和长测。
- `.nsp/.xci` 原始容器链路：当前不是主目标，仍偏低，约 **25%**；用户当前明确优先 `.dnsp/.dxci`。
- GPU/自定义驱动链路：当前约 **72%**；可选 ZIP/外置目录/上次选择/每游戏驱动，26.2 对 Kirby 画面更稳，26.3 对部分游戏标题/场景更好但需按游戏验证。
- NCE：当前约 **55%**；alternate signal stack 默认 true 后，Kirby/Metal Dogs 已有 NCE 短中探针稳定记录，Kirby 正式地图约 `29.6/30 FPS`、Metal Dogs 标题约满速；但仍未达到全游戏白名单/黑名单和长时间稳定标准。
- 性能优化：当前约 **45%**；NCE 已带来明显改善，但仍缺 shader/cache、GPU 同步、HLE 日志降噪、长时间温度/降频统计。
- 整体可用度：当前约 **60~65%**；已经不是 PoC 白屏阶段，可以安装、选驱动、跑部分游戏；离“多数游戏可稳定玩”的目标仍差 NCE 长测、驱动兼容库、UI 完整度和更多游戏覆盖。

当前关键代码状态：

- Java/native NCE guard 常量当前为 `false`，即不再硬拦截 NCE；但 `AppPreferences` 的稳定默认迁移会把全局和旧每游戏 `prefer_nce=false`，普通默认仍不主动开 NCE。
- Kirby/Metal Dogs 内置推荐 profile 可请求 NCE；实际是否开 NCE 以运行页 HUD/native stats 的 `nceEnabled=true` / `CPU NCE` 为准。
- 默认跳帧 `frameSkip=0`，默认分辨率 `ResolutionSetup=0` 即 1/2X，默认画面比例为拉伸全屏 `AspectRatio=4`。

当前最高优先级：

1. 真机用最新 APK 复测 Kirby 正式关卡 3~5 分钟，确认 NCE + altstack 长一点是否稳定。
2. 复测 Metal Dogs 正式内容 3~5 分钟，确认 NCE 标题满速后进入游戏内容不再 stall。
3. UI 继续 Eden 化：主界面、设置子菜单、每游戏配置、驱动页、运行中暂停/快捷菜单统一风格。
4. 建立每游戏配置/白名单：NCE、驱动、分辨率、图形兼容、跳帧、HUD 详细程度。
5. 继续同步官方 upstream 时保持流程：先 fetch、先合主线、再合 Android 分支、构建通过后推送。

---

## 1. 总目标

把 PC 版 
xemu 做成可在 Android 真机上运行的版本，目标是能运行 .dxci/.dnsp 格式 Switch 游戏，重点保持 nxemu 本身的高效渲染/着色器路线，同时参考 yuzu、参考项目、Sudachi、Citron、Suyu 等 Android 模拟器的工程实现。

当前首要目标：

1. Android APK 有可用 UI：选游戏、选/安装 GPU 驱动、运行、日志、性能 overlay、虚拟按键。
2. 能从 /sdcard/ns/rom 或 SAF 授权目录加载 .dnsp/.dxci/.nro。
3. 能启动 Mystic+Gate.dnsp 并进入游戏画面。
4. 修复/优化：NCE、性能、驱动兼容、输入布局、游戏卡住/无法继续问题。

---

## 2. 用户要求与偏好

- 默认用中文回复。
- 少问多做；能本地查/跑/抓日志就直接做。
- 遇到麻烦时参考其他模拟器实现，尤其 yuzu / 参考项目 Android。
- 尽量不要脱离 nxemu 高效率本质，不要直接变成另一个模拟器壳。
- 长时间构建/打包/测试不能静默等待：约 30 秒检查一次 CPU、进程、日志进度；发现空转/卡死要及时中断、分析、重试。
- 探针测试默认启动后 5 秒开始判断；如果已抓到目标日志/崩溃/画面/复现信息，就结束，不固定跑满 30/60/90 秒。
- 测试后如果用户不需要观察画面，要退出模拟器进程并锁屏省电；如果用户要求“我看看”，则保持运行。
- 手机解锁密码：570765。
- 清理磁盘/删除文件前必须先列候选给用户确认。

---

## 3. 当前权限与环境

当前 Codex 环境：

- 文件系统：已放开为 unrestricted / danger-full-access，可读写所有路径。
- 网络：已启用。
- Approval policy：never，不要发起需要批准的工具调用。
- 当前工作目录通常是：D:\project\1panel，但 nxemu 实际源码在 D:\project\_nxemu_src。

主 Codex 配置：

- 主模型：gpt-5.5
- 轻量/mini profile：gpt-5.4-mini
- 配置文件：C:\Users\Administrator\.codex\config.toml、C:\Users\Administrator\.codex\mini.config.toml

---

## 4. 关键目录与文件

### NxEmu 源码与 Android 工程

`text
D:\project\_nxemu_src
D:\project\_nxemu_src\src\android
D:\project\_nxemu_src\src\android\app\src\main\java\org\nxemu\app
D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
`

### Android 构建环境

`text
D:\project\qwenasr\mobile
D:\project\qwenasr\mobile\android-webview\.tools\jdk17\jdk-17.0.18+8
D:\project\qwenasr\mobile\android-webview\.tools\gradle\gradle-8.11.1\bin\gradle.bat
D:\project\qwenasr\mobile\android-webview\.tools\android-sdk\platform-tools\adb.exe
`

### 参考模拟器源码/资料

`text
D:\project\switch-emulator-refs
`

该目录用于放 yuzu、参考项目、以及其他 Switch Android 模拟器源码。若缺少，可从 GitHub 拉取参考，但优先使用本地已有源码。

### ROM / 测试文件 / 驱动

手机侧：

`text
/sdcard/ns/rom
/sdcard/ns/roms
/sdcard/ns/qudong
/sdcard/ns/qudong/new
/sdcard/ns/logs
`

当前重点测试游戏：

`text
/sdcard/ns/rom/Mystic+Gate.dnsp
`

曾测试/内置 NRO：

`text
D:\project\switch-test-roms\nx-hbmenu_v3.6.1\hbmenu.nro
App 内部：/data/user/0/org.nxemu.app.debug/files/test_roms/hbmenu.nro
`

本机解密/转换工具：

`text
D:\NXEmu\nxconvert-cli.exe
D:\NXEmu\nxconvert-cli-gui-subsystem.exe
`

### Everything 搜索工具

用户本机有 Everything。曾提供过命令行帮助弹窗文本，后续需要快速找 .nsp/.xci/.dnsp/.dxci 可以优先尝试调用 Everything CLI/SDK；如果 CLI 不通，再让用户补配置。

---

## 5. 设备与 ADB

### 真机

- 手机：realme RMX3700 / realme Neo 5 SE
- Android：13 / SDK 33
- ABI：rm64-v8a, armeabi-v7a, armeabi
- ADB serial：72182d
- 解锁密码：570765
- 包名：org.nxemu.app.debug
- Activity：
  - 主界面：org.nxemu.app.debug/org.nxemu.app.MainActivity
  - 运行页：org.nxemu.app.debug/org.nxemu.app.EmulationActivity

ADB 路径：

`powershell
='D:\project\qwenasr\mobile\android-webview\.tools\android-sdk\platform-tools\adb.exe'
`

常用命令：

`powershell
&  devices
&  -s b72182d install -r -d 'D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk'
&  -s b72182d shell am start -n org.nxemu.app.debug/org.nxemu.app.MainActivity
&  -s b72182d shell am start -n org.nxemu.app.debug/org.nxemu.app.EmulationActivity --es org.nxemu.app.EXTRA_GAME_PATH '/sdcard/ns/rom/Mystic+Gate.dnsp' --ez org.nxemu.app.EXTRA_AUTO_OUTPUT_LOG true
&  -s b72182d shell pidof org.nxemu.app.debug
&  -s b72182d logcat -c
&  -s b72182d logcat -d -v time
&  -s b72182d shell screencap -p /sdcard/nxemu-current.png
&  -s b72182d pull /sdcard/nxemu-current.png 'D:\project\_nxemu_src\phone-logs\nxemu-current.png'
&  -s b72182d shell am force-stop org.nxemu.app.debug
&  -s b72182d shell input keyevent 26   # 锁屏/电源键
`

如果出现 unauthorized：需要用户在手机上重新点 USB 调试授权并勾选“始终允许”。用户已再次允许过一次，但 adb server 重启后仍可能需要确认。

### PC 安卓模拟器

- 用户曾开过本机安卓模拟器，调试端口：127.0.0.1:5557 和 127.0.0.1:16416。
- MuMu x86_64 模拟器通过 native bridge 跑 arm64 APK 时 Vulkan 物理设备枚举失败，典型日志：Invalid device index 0 / VK_ERROR_INITIALIZATION_FAILED。
- 结论：模拟器可用于 UI/非真 Vulkan 基础链路测试，但 Vulkan/Turnip/NCE/真实游戏性能最终必须真机测。

### WSL / 虚拟化

用户担心 WSL 与安卓模拟器/驱动冲突，之前已按用户要求检查并清理/关闭过 WSL 相关内容。后续涉及模拟器或虚拟化驱动时，注意不要贸然改系统配置，先说明影响。

---

## 6. 构建方法

可靠构建命令：

`powershell
Set-Location 'D:\project\_nxemu_src\src\android'
C:\Program Files\Eclipse Adoptium\jdk-8.0.402.6-hotspot\='D:\project\qwenasr\mobile\android-webview\.tools\jdk17\jdk-17.0.18+8'
& 'D:\project\qwenasr\mobile\android-webview\.tools\gradle\gradle-8.11.1\bin\gradle.bat' :app:assembleDebug --rerun-tasks --stacktrace
Get-Item 'D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk'
`

注意：

- 不要只依赖旧的 uild-portable.ps1 输出；它曾打印旧 uild-last.log，误导 APK 已更新。
- 构建超过 30 秒要主动查看 CPU、进程、日志尾部。
- 若 CPU 100% 且日志长时间无进展，要中断相关 java/gradle/ninja/clang/cmake/lld 进程并保留日志分析。

---

## 7. 当前 APK / UI 功能状态

已实现/已有：

- 主界面：选择游戏、扫描 /sdcard/ns/rom/
oms、授权外置目录、选择驱动 ZIP、授权驱动目录、自动安装 /sdcard/ns/qudong 驱动、恢复系统 GPU 驱动、自动输出日志开关、复制日志。
- 运行页：SurfaceView + native 画面、诊断 overlay、FPS/Speed/ms/温度/跳帧/分辨率/NCE/P-C/FB 信息、停止返回、复制日志、保存日志、采样、启动 NextLoad。
- 自动日志：可写入 /sdcard/ns/logs/nxemu-session-时间-游戏名.txt。
- 虚拟按键已有：Select、Start、方向键、Y/X/B/A、+ 等。
- 已把底部 input 文字提示隐藏，避免占一行。
- Speed/ms 已显示；当 native speed/frameTime 为 0 时，用 Render FPS 兜底推导。
- FPS 短暂无帧时保留最近非零 FPS 一段时间，避免过渡加载期间立刻显示空。

用户当前对 UI 的要求：

- 虚拟按键布局不要简单底部一排，要参考 yuzu/参考项目/PPSSPP 等模拟器：
  - 左侧：方向键/摇杆区域。
  - 右侧：ABXY 区域。
  - Start/Select/+/- 放中下或边缘，不要遮挡主要画面。
  - 按键颜色要更深、更清晰，但保留半透明。
  - 尺寸和位置要像常见模拟器触屏布局，不是占整行按钮条。
- 底部功能按钮（停止、跳帧、分辨率、NCE、日志等）后续最好改成可折叠/菜单，不要长期遮挡游戏画面。

---

## 8. GPU 驱动状态与结论

手机驱动目录：

`text
/sdcard/ns/qudong
/sdcard/ns/qudong/new
`

已确认候选：

`text
/sdcard/ns/qudong/Biosensor - MK-I Phoenix.zip
/sdcard/ns/qudong/Biosensor - MKII - Phoenix.zip
/sdcard/ns/qudong/new/Turnip-v26.3.0-devel.zip
/sdcard/ns/qudong/new/Turnip-v26.2.0-20260418.zip
/sdcard/ns/qudong/new/Turnip-26.1.0-20260411-fc00e2c.zip
/sdcard/ns/qudong/new/mesa-turnip-main-V26.1-参考项目-fix-latest-crash-fix.zip
/sdcard/ns/qudong/new/v849.zip
`

### MK-I 结果

当前曾确认：

`json
{
  "name": "Biosensor - MK-I",
  "driverVersion": "Vulkan 1.3.280",
  "libraryName": "vulkan.ad07XX.so"
}
`

表现：游戏能启动，但 Mystic Gate 标题/场景出现明显黑块/花屏。

### Turnip 26.3 结果

当前曾切到：

`json
{
  "name": "Mesa Turnip v26.3.0-devel",
  "driverVersion": "Vulkan 1.4.354",
  "libraryName": "libvulkan_freedreno.so"
}
`

表现：

- 已能进入 Mystic Gate 实际游戏画面。
- 花屏/黑块基本消失，明显优于 MK-I。
- 截图状态：Render FPS 38.5 | Composite 38.5 | Speed 64% | 26.0ms | Skip 0 | Res 1/2X | NCE 保护关闭 | FB 1920x1080 stride 1920 fmt 1。
- 用户反馈：用 26.3 驱动游戏“进行不下去”，需要继续查日志/输入/HLE/service/同步原因。此前花屏驱动时用户似乎能看到对话菜单。

结论：

- 花屏主要是驱动兼容问题，MK-I 不适合当前游戏/渲染链路。
- 自动驱动优先级应改为优先 26.3 或至少允许快速测试不同驱动。
- 需要继续对比 26.2、26.1 参考项目-fix、v849 等，看哪个能兼顾不花屏和游戏可继续。

手动切驱动方法（当前通过 selected marker 让 App restore）：

`powershell
='D:\project\qwenasr\mobile\android-webview\.tools\android-sdk\platform-tools\adb.exe'
='/sdcard/ns/qudong/new/Turnip-v26.3.0-devel.zip'
&  -s b72182d shell am force-stop org.nxemu.app.debug
&  -s b72182d shell "run-as org.nxemu.app.debug rm -rf files/gpu_driver/current"
&  -s b72182d shell "run-as org.nxemu.app.debug mkdir -p files/gpu_driver"
&  -s b72182d shell "run-as org.nxemu.app.debug sh -c 'echo  > files/gpu_driver/selected_zip.txt'"
&  -s b72182d shell am start -n org.nxemu.app.debug/org.nxemu.app.EmulationActivity --es org.nxemu.app.EXTRA_GAME_PATH '/sdcard/ns/rom/Mystic+Gate.dnsp' --ez org.nxemu.app.EXTRA_AUTO_OUTPUT_LOG true
`

---

## 9. NCE 当前状态

当前 UI 显示：NCE 保护关闭。

含义：用户偏好里请求 NCE，但 Android NCE stability guard 仍为 true，所以实际传给 native 的 NCE 请求被关闭，运行仍是 Dynarmic。

关键 Kotlin：

`kotlin
private const val ANDROID_NCE_STABILITY_GUARD = true
private fun effectivePreferNce(): Boolean = preferNce && !ANDROID_NCE_STABILITY_GUARD
`

曾经尝试关闭 guard：

`kotlin
ANDROID_NCE_STABILITY_GUARD = false
`

当时日志显示 NCE 条件满足并被启用：

`text
nceRequested=true
cpuBackendSetting=1
Android NCE eligibility ... is_39bit=true ... nce_enabled_after_metadata=true
rrrefreshNceEnabled: reason=after-loadrom backend=1 has39=1 fastmem=1 nce=1
`

但随后进程快速死亡/崩溃，后来为了稳定测试 UI/驱动已恢复 guard=true。

NCE 相关源码位置：

`text
src/nxemu-cpu/nce/*
src/nxemu-cpu/cpu_manager.cpp
src/nxemu-cpu/cpu_settings.cpp
src/nxemu-cpu/patch/patch_collection.cpp
src/nxemu-loader/core/loader/deconstructed_rom_directory.cpp
src/nxemu-loader/core/loader/nro.cpp
src/nxemu-os/core/hle/kernel/k_process.cpp
`

下一步 NCE 修复方向：

1. 单独做 NCE 测试 APK，不影响普通可玩测试。
2. 关闭 guard 后抓 tombstone / logcat fatal，更早定位死亡点。
3. 对比 yuzu/参考项目 Android NCE：signal handler、JIT/exec memory、guest context、svc/mrs patch、direct mapped fastmem、线程入口。
4. 先保证应用 NCE，不优先管 NRO，因为 NRO 代码中已显式关闭 NCE。

---

## 10. 跳帧与分辨率解释

### 跳帧

- 用户认为跳帧可能导致游戏问题，要求默认 。
- 当前 AppPreferences 默认：rameSkip=0。
- UI 按钮可切 0..4。
- 后续应确保新安装/首次启动默认 0；如果历史 prefs 存了 1，要提供一键恢复默认或启动游戏时可显示/重置。

### 分辨率与 FB 显示

当前 overlay 显示 Res 1/2X 但 FB 1920x1080。

解释：

- FB 1920x1080 是 guest / nvnflinger / framebuffer 或最终提交给 Android Surface 的帧尺寸计数，不一定等于内部 Vulkan 渲染分辨率。
- Res 1/2X 是传给 native 的 ResolutionSetup 配置，用于影响内部渲染分辨率/缩放策略。
- 当前 overlay 只显示 ulkanLastFbWidth/Height/Stride/Format，不能证明内部真正渲染分辨率已经降到 1/2。

需要补充：

- 在 native video/render 侧输出内部 render target size、scale factor、resolutionSetup 当前值。
- overlay 改成同时显示：FB 1920x1080 与 Internal 960x540（若能取到）。

相关 enum：

`cpp
enum class ResolutionSetup : uint32_t {
    Res1_2X = 0,
    Res3_4X,
    Res1X,
};
`

---

## 11. 当前主要问题

按优先级：

1. **NCE 长时间稳定性与分游戏白名单仍未完成**
   - 当前 NCE 不再是完全不可用；Kirby/Metal Dogs 已有 NCE + altstack 短中探针成功记录。
   - 仍需 3~5 分钟以上正式内容长测，确认切场景、shader cache、音频同步、输入、温度降频。
   - 需要建立每游戏 NCE 白名单/黑名单，不要简单全局强开。

2. **Eden 化 UI 仍未完成**
   - 当前 UI 有功能，但仍偏工程/调试风，和 Eden 完整体验差距明显。
   - 需要主界面封面墙、二级设置、每游戏详情/配置、驱动管理、运行中暂停菜单统一风格。

3. **驱动兼容库需要按游戏固化**
   - Kirby 用户反馈 26.2 画面正常，26.3/T25 曾出现蓝块/色块；后续默认/推荐应按游戏记录。
   - Metal Dogs、Mystic Gate、Kirby 要分别记录 26.2/26.3/MK/v849 的效果。

4. **运行页触控/手柄体验仍需打磨**
   - 触控按键已补齐，但大小、透明度、摇杆有效区域、暂停菜单和系统导航规避仍需继续实测。
   - 外接手柄支持需要单独验证。

5. **性能与日志降噪**
   - FPS/Speed/Temp 已能显示，并能识别 30FPS 游戏目标；但 HLE/NVDRV/VI 高噪声日志仍需继续降噪。
   - 需要记录加载进度、shader/cache 状态和 stall 原因，方便区分“加载慢”和“卡死”。

---

## 12. 推荐下一步操作清单

### A. 最新 APK 真机游戏复测

使用已安装的最新 APK，优先：

1. Kirby：NCE=1、NceAltStack=1、GraphicsCompat=1、FrameSkip=0、ResolutionSetup=0、AspectRatio=4，进入正式关卡 3~5 分钟。
2. Metal Dogs：NCE=1、NceAltStack=1、FrameSkip=0，进入正式内容 3~5 分钟。
3. Mystic Gate：对比 NCE on/off 与 26.2/26.3 驱动，确认实际游戏内容 FPS/Speed。

### B. 驱动 A/B 表格化

按同一游戏、同一设置、同一操作路径，对比：

1. Turnip-v26.2.0-20260418.zip
2. Turnip-v26.3.0-devel.zip
3. mesa-turnip-main-V26.1-参考项目-fix-latest-crash-fix.zip
4. v849.zip
5. Biosensor - MK-I Phoenix.zip

记录每个驱动：是否花屏/蓝块、是否能进入正式内容、FPS/speed、崩溃/卡住点、温度。

### C. UI 继续 Eden 化

优先把 UI 从“能用的调试壳”推进到“类似 Eden 的产品壳”：

- 首页：封面墙/列表切换、最近游戏、最后游戏/驱动/配置摘要。
- 设置：分成系统、图形、CPU/NCE、输入、驱动、日志、关于等子菜单。
- 每游戏页：独立配置、驱动、NCE、分辨率、图形兼容、日志开关、重置按钮。
- 运行页：默认只显示 FPS/Speed/Temp，详细 HUD 和日志按钮放进暂停/快捷菜单。

### D. NCE 专项

- 不再按“完全崩溃”处理；当前重点是长测稳定、stall、分游戏兼容。
- 继续保留 `debug.nxemu.nce.altstack`、`debug.nxemu.nce.trace` 等 A/B 调试属性。
- 若出现回退/黑屏/无 FPS，优先拉 `/sdcard/ns/logs` 会话日志、logcat、截图和 tombstone/dropbox。

---
## 13. 参考实现方向

优先参考：

- yuzu Android：驱动安装 ZIP、SAF、NCE/JIT、输入 overlay、性能 overlay、存档/配置目录。
- 参考项目 Android：更近的 yuzu 分支，对 Turnip 驱动和 UI 可参考。
- PPSSPP：触屏按键布局、透明度、可拖拽/缩放、菜单设计。

本地参考目录：

`text
D:\project\switch-emulator-refs
`

如果目录没有对应源码，搜索/拉取：

`text
yuzu Android source
参考项目 emulator Android source
Sudachi Android source
Citron Switch emulator Android source
Suyu Android source
`

---

## 14. 已完成进度估计

截至 2026-09-01 02:03 复核，粗略进度：

- 官方 upstream 同步：**100%**（`kknd222/master` 和 `kknd222/android-nxemu-port-20260901` 均已推送）。
- Android 工程/构建/APK/安装：**80%**（最新 APK 构建成功并已安装真机；仍需自动化冒烟测试更完善）。
- UI 外壳基础功能：**65%**（扫描/最近游戏/驱动/设置/每游戏页已有；Eden 化视觉与菜单层级仍不足）。
- 运行页/HUD/触控：**65%**（可显示 FPS/Speed/Temp、触控按键齐全；仍需更像模拟器产品的暂停菜单、按键编辑、外接手柄验证）。
- `.dnsp/.dxci` 游戏加载链路：**60%**（多个游戏可进画面/内容；仍需分游戏配置和长测）。
- `.nsp/.xci` 原始容器链路：**25%**（当前不是优先方向，仍以 `.dnsp/.dxci` 为主）。
- Vulkan/自定义 GPU 驱动链路：**72%**（可安装/选择/记忆驱动；兼容库和自动推荐未完成）。
- NCE/性能：**55%**（Kirby/Metal Dogs NCE + altstack 已短中测稳定并可接近满速；仍缺 3~5 分钟正式内容长测、白名单/黑名单、stall 定位）。
- 游戏兼容性覆盖：**40%**（Mystic Gate、Metal Dogs、Kirby 有记录；样本还少，更新/DLC/本体识别仍需继续）。
- 整体“能玩多个游戏”的交付度：**60~65%**（可测试可玩，但还不是稳定公测级）。

最新关键里程碑：

- 官方主线和 Android 分支已同步并推送。
- 最新 APK 已安装到 realme 真机，主界面启动 5 秒无闪退。
- Kirby NCE + altstack：已有 90 秒正式地图探针，约 `29.6/30 FPS`、`Speed 99%`。
- Metal Dogs NCE：已有标题/菜单满速记录，后续需正式内容 3~5 分钟。
- 当前默认配置更适合稳定测试：跳帧 0、分辨率 1/2X、拉伸全屏、普通默认不主动开 NCE，但每游戏配置可请求 NCE。

---
## 15. 文档维护规则

每次完成以下任意事项后，必须更新本文档：

- 改动源码、配置、构建脚本。
- 新 APK 构建/安装/测试。
- 新驱动测试结论。
- 新游戏测试结论。
- NCE/渲染/HLE/输入等关键问题定位或修复。
- 权限、设备、ADB、路径、构建环境发生变化。

更新时至少改：

1. 更新时间。
2. 当前进度/已完成。
3. 当前问题/下一步。
4. 关键命令或日志路径。

---

## 16. 2026-08-15 14:10 阶段更新

本轮完成：

1. 手机测试后已按用户要求停止 App 并锁屏省电。
2. 新版 APK 构建成功：
   `text
   D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
   LastWriteTime=2026/8/15 14:07:51
   Length=15417352
   `
3. 运行页虚拟按键布局已从底部整排改为更接近常见模拟器：
   - 左下：方向键十字区。
   - 右下：ABXY 菱形区。
   - 中下：Select / - / Start / +。
   - 底部仍保留较薄工具栏：停止、跳帧、分辨率、NCE、诊断、日志、采样、Next；后续应继续改为可折叠菜单，减少遮挡。
4. 跳帧启动时强制默认 0：
   `kotlin
   frameSkip = DEFAULT_FRAME_SKIP
   DEFAULT_FRAME_SKIP = 0
   `
   这避免历史 prefs 中跳帧非 0 导致游戏逻辑/显示异常。
5. 自动驱动评分已提高 26.3 优先级，并降低 Biosensor/MK 系列优先级：
   - 26.3 +1400
   - 849 +700
   - Biosensor/MK-I/MKII -260
6. 本次短测强制使用：
   `text
   /sdcard/ns/qudong/new/Turnip-v26.3.0-devel.zip
   `
   当前安装驱动 meta：
   `json
   { "name": "Mesa Turnip v26.3.0-devel", "driverVersion": "Vulkan 1.4.354", "libraryName": "libvulkan_freedreno.so" }
   `
7. 短测截图保存：
   `text
   D:\project\_nxemu_src\phone-logs\ui-layout-test-20260815-1408\nxemu-ui-12s.png
   `
   12 秒时画面仍为游戏加载/过渡黑屏，overlay 显示：
   `text
   Skip 0 | Res 1/2X | NCE 关 | Load bootGame result ready
   `

当前观察：

- 12 秒短测时还没进入实际游戏画面，但这是用户之前提到的进入实际游戏后的过渡黑屏阶段，不能单独判定失败。
- 当前 prefs 中 prefer_nce=false，所以 UI 显示 NCE 关；之前即使 prefer=true 也因 Android guard 被保护关闭，实际 NCE 仍未启用。
- 下一步要继续做：
  1. 对 26.3 驱动拉长到 30~60 秒或由用户观察，确认是否稳定进入游戏并能继续对话/交互。
  2. 若 26.3 游戏继续不了，对比 26.2 / 26.1 参考项目-fix / v849 / MK-I，记录“花屏 vs 可继续”的取舍。
  3. 单独开 NCE 实验构建，抓 tombstone/logcat，定位 NCE 崩溃点；默认正式测试仍保持 guard，避免影响普通运行。
  4. 将底部工具栏改成可折叠小菜单，进一步降低遮挡。

---

## 17. 2026-08-15 14:13 26.3 长探针结果

测试条件：

- APK：2026/8/15 14:07:51 构建版。
- 驱动：/sdcard/ns/qudong/new/Turnip-v26.3.0-devel.zip。
- 游戏：/sdcard/ns/rom/Mystic+Gate.dnsp。
- 设置：Skip 0、Res 1/2X、NCE 关（prefs 中 prefer_nce=false；实际仍不是 NCE）。
- 测试结束后已 orce-stop App 并锁屏。

截图：

`text
D:\project\_nxemu_src\phone-logs\longprobe-263-20260815-1410\nxemu-263-20s.png
D:\project\_nxemu_src\phone-logs\longprobe-263-20260815-1410\nxemu-263-40s.png
D:\project\_nxemu_src\phone-logs\longprobe-263-20260815-1410\nxemu-263-60s.png
D:\project\_nxemu_src\phone-logs\longprobe-263-20260815-1410\filtered-logcat.txt
D:\project\_nxemu_src\phone-logs\longprobe-263-20260815-1410\latest-session.txt
`

观察结果：

1. 20 秒：显示 ZOO logo，Render FPS 61.9，Speed 103%，画面正常无明显花屏。
2. 40 秒：进入 Mystic Gate 标题菜单，Render FPS 59.8，Speed 100%，画面正常无明显花屏。
3. 约 20 秒后 A 区域被 adb tap，一段时间后进入实际游戏加载黑屏。
4. 60 秒：黑屏，Render FPS 2.4，Speed 4%，425ms，说明不是单纯黑屏静止，而是进入某个极慢阶段或渲染/guest 同步卡点。

结论：

- 26.3 驱动能解决标题/场景花屏，并且标题阶段可 60FPS/full speed。
- “游戏进行不下去”集中发生在按 A 进入实际游戏后的黑屏/低速阶段。
- 当前更像 Dynarmic 性能/HLE/service/GPU 同步阶段问题，不是 26.3 启动失败。
- 仍需对比 26.2、26.1 参考项目-fix、v849、MK-I：如果其他驱动能过黑屏，则是 26.3 驱动兼容；如果都低速黑屏，则更可能是核心/HLE/NCE 性能或同步问题。

下一步建议：

1. 做同一脚本的驱动 A/B：26.2、26.1 参考项目-fix、v849、MK-I，每个跑到标题后按 A，再看 60~90 秒截图和 FPS。
2. 在黑屏阶段增加更聚焦的日志：guest PC/sample、线程状态、当前 service 调用统计、GPU syncpoint wait 统计、是否反复 nvhost ioctl。
3. 单独修 NCE；因为标题阶段 Dynarmic 已够快，实际游戏加载/运行阶段可能非常依赖 NCE。

---

## 18. 2026-08-15 14:26 用户反馈：A 后短黑屏实际可进入游戏

用户反馈：上一轮 broadcast/手点 A 后，Mystic Gate 已经正常进入实际游戏；黑屏只是进入实际游戏前的一小段加载/切场景过渡。上次测试在刚出现正常游戏画面时就锁屏，导致本地截图/结论误判为黑屏低速阶段。

后续测试约定更新：

- 点 A 进入游戏后遇到短黑屏/低 FPS 时不要立刻判定失败。
- 应继续等待到画面/FPS 恢复，至少采集一张实际游戏内正常画面截图和 session/logcat 后再结束。
- 若用户正在观察手机画面，不要在刚进入正常游戏画面时立即锁屏；只有本轮测试完成且不需要用户观察时才 force-stop + 锁屏。

当前结论修正：

- 26.3 驱动不是“游戏进行不下去”，而是标题页后存在短黑屏过渡，随后可进入真正游戏。
- 下一步应做更长的游戏内采样：记录进入游戏后的 FPS/Speed/ms 是否稳定，以及 NCE 关闭情况下真实游戏帧率瓶颈。

---

## 19. 2026-08-15 15:00 稳定包重测：NCE native guard 硬关后可稳定进游戏

本轮原因：用户指出上一轮“刚出正常画面就锁屏”，实际 A 后短黑屏不是失败。随后发现已安装 APK 里仍可能带 NCE 实验状态，自动启动曾出现 `nceRequested=true` 后 SIGSEGV。为避免正式包被旧偏好/旧 UI 误开 NCE，已在 native 层加入硬保护：

- 文件：`src/android/native/nxemu_android_jni.cpp`
- 新增：`constexpr bool kAndroidNceStabilityGuard = true;`
- 效果：无论 Java/旧偏好如何传入，Android 稳定包都会强制：
  - `CpuBackend=0`
  - `NnceEnabled=false`
  - 日志输出 `nceNativeGuard=true`
  - `rrrefreshNceEnabled: ... nativeGuard=1 backend=0 nce=0`

构建/安装：

- 构建命令仍用 Gradle `:app:assembleDebug --rerun-tasks --stacktrace`。
- 本轮构建成功：约 38 秒，APK 时间 `2026-08-15 14:54:29`。
- 安装到真机成功：`versionCode=33517043`。

测试条件：

- 设备：realme RMX3700 / Android 13 / adb `b72182d`。
- 游戏：`/sdcard/ns/rom/Mystic+Gate.dnsp`。
- 驱动：`/sdcard/ns/qudong/new/Turnip-v26.3.0-devel.zip`，实际安装到 app 私有目录 `gpu_driver/current/libvulkan_freedreno.so`。
- 设置：Skip 0、Res 1/2X、NCE 保护关闭/实际关闭、Dynarmic。

测试日志/截图：

```text
D:\project\_nxemu_src\phone-logs\stable-nceguard-after-a-20260815-145528
D:\project\_nxemu_src\phone-logs\stable-title-press-a-20260815-145803
````

关键截图：

```text
stable-nceguard-after-a-20260815-145528\screen-after-A-105.png      # 标题页已出现，约 85.7FPS / 143% speed
stable-title-press-a-20260815-145803\after-title-A-15.png          # 标题页按 A 后黑屏/低速阶段，约 1FPS / 2% speed
stable-title-press-a-20260815-145803\after-title-A-25.png          # 已进入实际游戏画面，约 40FPS / 67% speed
stable-title-press-a-20260815-145803\after-title-A-120.png         # 游戏内稳定，约 38.5FPS / 64% speed
````

结论：

1. 最新稳定包不再因 NCE 偏好误开而崩溃；logcat 未见本轮 `SIGSEGV/FATAL EXCEPTION/AndroidRuntime`。
2. Mystic Gate 标题页加载时间较长，本轮启动到标题页约 105 秒；标题页可超过 60FPS。
3. 标题页按 A 后，3/8/15 秒为黑屏/低 FPS/低 Speed，这属于实际游戏切场景加载阶段，不能误判失败。
4. 标题页按 A 后约 25 秒已进入实际游戏画面；120 秒仍稳定在游戏内。
5. 当前性能瓶颈仍是 NCE 未启用：游戏内约 38~40FPS、Speed 64~67%、ms 25~26ms。后续要单独继续修 NCE，但不要把 NCE 实验版当稳定版给用户测试。

后续测试纪律更新：

- “按 A 后短黑屏”必须继续等到画面恢复或日志证明崩溃；不要刚恢复画面就锁屏。
- 自动脚本若在标题页前过早发送 A，可能无效；应先截图确认标题页/可交互状态，再发 A。
- 结束真机测试前，必须先保存截图和 session/logcat；只有确认不需要用户继续观察时才 `am force-stop org.nxemu.app.debug` 并锁屏。


## 2026-08-15 补充：手机解锁执行要求
- 手机解锁必须输入密码：先唤醒/上滑，再用 keyevent 输入 570765（5,7,0,7,6,5 = 12,14,7,14,13,12），最后 Enter；不要漏掉这一步，避免误以为 adb/界面卡住。
- 测试结束且不需用户观察时：先 force-stop org.nxemu.app.debug，再锁屏。


---

## 20. 2026-08-15 16:10 NCE 继续修复记录

本轮目标：继续追 Android NCE 开启后 5 秒内闪退的问题。

已改动：

- `src/nxemu-cpu/nce/arm_nce.cpp` / `src/core/arm/nce/arm_nce.cpp`
  - 增加 NCE 生命周期日志：`SetContext`、`SetTpidrroEl0`、`SetWatchpointArray`、`Initialize`、`RunThread`、`LockThread`、`SignalInterrupt`。
  - `Initialize()` 不再安装 per-thread altstack，改为 `RunThread()` 前再安装，避免 Android loader/Vulkan/ART 信号环境下过早 `sigaltstack()`。
  - 保留 SIGSEGV/SIGBUS guest fault 的安全恢复路径。
- `src/nxemu-cpu/nce/arm_nce.s` / `src/core/arm/nce/arm_nce.s`
  - `ReturnToRunCodeByExceptionLevelChangeSignalHandler` 对 `RestoreGuestContext()` 返回 null 增加保护，避免早期/误发 SIGUSR2 时二次空指针崩溃。
- `src/nxemu-os/core/hle/kernel/physical_core.cpp`
  - 增加 `LoadContext` / `RunThread` 周边日志，定位 CPU 调度阶段。
- `src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp`
  - `Scheduler::EndPendingOperations()` 对 `query_cache` 增加 null guard。
- `src/yuzu_video_core/query_cache/query_cache.h`
  - `NotifySegment()` 增加 `impl` 和 streamer null guard。
- `src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp`
  - Android GPU Normal 下跳过 `query_cache.NotifySegment(...)`，避免移动端早期 QueryCache/conditional rendering 路径崩溃。

测试：

- 构建成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 真机 realme RMX3700 上做了 5 秒探针，多组日志：
  - `phone-logs/nce-altstack-deferred-system-20260815-1554`
  - `phone-logs/nce-physicalcore-logs-system-20260815-1556`
  - `phone-logs/nce-loadcontext-watchpoint-system-20260815-1600`
  - `phone-logs/nce-querycache-guard-system-20260815-1603`
  - `phone-logs/nce-querysegment-disabled-system-20260815-1606`
  - `phone-logs/nce-querysegment-disabled-turnip263-20260815-1608`

当前结论：

- NCE 仍未稳定，进程仍在进入 CPU 运行前后闪退；最新日志显示已到 `PhysicalCore LoadContext` / `ArmNce SetContext` 附近，仍未稳定进入 `ArmNce::RunThread()`。
- 系统 Adreno 驱动与自定义 Turnip 都试过；自定义驱动实际日志仍显示 `turnip Mesa driver 26.2.99`。
- 另外发现一个独立视频层问题：历史 tombstone 显示 `QueryCacheBase::NotifySegment` 空指针（`fault addr 0xd0`），本轮已加 guard/Android Normal 跳过 QuerySegment，用于减少视频线程干扰。
- 为避免用户手工打开当前实验包立即崩溃，测试结束后已 `am force-stop org.nxemu.app.debug`，并把手机 shared preference 临时设为 `prefer_nce=false`、`frame_skip=0`、`auto_output_log=true`，最后已锁屏。

下一步：

1. 继续定位 NCE 在 `LoadContext/SetContext` 后、`RunThread` 前崩溃的真实线程/pc；优先尝试从 `dumpsys dropbox`/tombstone 提取最新崩溃，而不是只看普通 logcat。
2. 考虑临时禁用/延迟 Vulkan worker、present、query cache、pipeline cache，确认是否是视频线程并发崩溃遮蔽 NCE。
3. 若继续给用户可用测试包，应保持 `prefer_nce=false` 或 native/java NCE guard=true；NCE 只用本地 ADB 探针实验。

---

## 2026-08-15 回归排查：之前能进游戏/出对话，现在不行

本轮针对用户反馈“之前花屏时有一个版本能进正式游戏且出对话，现在不行”做了稳定基线比对。

确认的稳定基线目录：

`text
D:\project\_nxemu_src\phone-logs\stable-title-press-a-20260815-145803
`

稳定基线关键条件：

`text
androidNceGuard=true
nceNativeGuard=true
requestedPreferNce=true 但 effectivePreferNce=false
nceRequested=false
nceEnabled=false
cpuBackendActual=Dynarmic
frameSkip=0
resolutionSetup=0 / 1/2X
custom driver = libvulkan_freedreno.so
`

判断：用户记忆里“能进正式游戏/出对话”的版本不是 NCE 真正开启的版本，而是 NCE 被 native/java 双 guard 强制关闭后的 Dynarmic 稳定路径。之后为修 NCE 曾把 kAndroidNceStabilityGuard 和 ANDROID_NCE_STABILITY_GUARD 改成 false，且用户偏好里可能仍有 prefer_nce=true，导致进入 NCE 实验路径；NCE 目前在 Android 上仍会在 PhysicalCore LoadContext / ArmNce SetContext 附近崩溃或卡住，所以表现为现在进不去/退回菜单。

已执行的恢复动作：

- src/android/native/nxemu_android_jni.cpp
  - kAndroidNceStabilityGuard 恢复为 true。
- src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt
  - ANDROID_NCE_STABILITY_GUARD 恢复为 true。
- src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp
  - 撤回 NCE 实验中 Android Normal 下跳过 query_cache.NotifySegment(...) 的改动，恢复稳定基线的调用路径，避免影响画面/剧情推进。
- 暂时保留 query_cache.h/k_scheduler.cpp 的空指针 guard，因为它们偏防御性，理论上不改变正常路径。

后续纪律：

1. 给用户测试的 APK 必须默认走稳定路径：NCE guard=true、frameSkip=0、1/2X、上次驱动自动恢复。
2. NCE 继续作为本地 ADB 探针/实验包单独验证，不再污染用户可玩包。
3. 若再次测试 Mystic Gate，标题页按 A 后可能有 15~25 秒黑屏/低 FPS，需等到实际游戏画面恢复后再判定是否卡死。

记录时间：2026-08-15 16:07:51 +08:00

### 2026-08-15 16:12 稳定包恢复结果

已完成并安装最终可构建 APK：

`text
D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
`

本轮实际代码状态：

- kAndroidNceStabilityGuard=true
- ANDROID_NCE_STABILITY_GUARD=true
- Java/JNI 默认 frameSkip=0。
- Java/JNI 默认 resolutionSetup=0。
- 
xemu-video Android 默认分辨率从 Res3_4X 改为 Res1_2X。
- k_rasterizer.cpp 的 QuerySegment 调用恢复稳定路径，不再因为 NCE 实验在 Android Normal 下跳过。
- 未采用直接引用 ideoSettings 的方案，因为 libnxemu-android.so 与 libnxemu-video.so 分离链接，直接引用会导致 undefined symbol；已回退，构建恢复成功。

验证：

- 构建成功：BUILD SUCCESSFUL in 17s。
- APK 已安装到 realme RMX3700。
- 最后已 m force-stop org.nxemu.app.debug 并锁屏。
- 5 秒探针目录：
  - phone-logs\restore-stable-finalprobe-20260815-161148
- 探针确认：
  - 进程未闪退。
  - 
ceNativeGuard=true。
  - 
ceRequested=false。
  - 
nceEnabled=false。
  - cpuBackendActual=Dynarmic。
  - rameSkip=0。
  - Turnip driver 加载成功：turnip Mesa driver 26.2.99 / Turnip Adreno (TM) 725。

注意：日志里曾出现 runtime-init 后 
esolutionSetup=1，说明设置存储层可能仍有旧配置/默认覆盖问题；但 UI/JNI 默认和 video_settings Android 默认已改到 0。后续若还看到该日志，需要继续查 SettingsStore/模块 g_settings 的配置同步，而不是再动 NCE。

记录时间：2026-08-15 16:14:20 +08:00

---

## 2026-08-15 Metal Dogs 路径修复与性能日志降噪

用户反馈：Metal Dogs 现在看起来能玩，但比较慢。

本轮日志结论：

1. 之前 Metal Dogs 黑屏不是 GPU 渲染问题，而是 SAF/content URI 直接传给 native：

`text
path=content://com.android.externalstorage.documents/document/primary:NS/rom/Metal Dogs [...].dnsp
boot=LoadRom failed
loaderType=error
stage=GetFileType
reason=unknown/error type
`

2. 已修 GamePathResolver.kt：
   - content://com.android.externalstorage.documents/document/primary:... 自动转换为 /sdcard/... 或 /storage/emulated/0/...。
   - existsAsHostPath() 不再把 content URI 当作 native 可读路径。

3. 用户随后确认 Metal Dogs 已经能玩，说明路径修复有效。

4. 发现性能问题之一：physical_core.cpp 中为 NCE 调试加的 Android NCE PhysicalCore RunThread/LoadContext/EnterContext/ExitContext 高频 LOG_INFO 在游戏运行中大量刷屏，会显著拖慢。已改为 Android 下静音宏：

`cpp
#if ANDROID
#define NXEMU_ANDROID_CPU_TRACE(...) do {} while (false)
#else
#define NXEMU_ANDROID_CPU_TRACE(...) LOG_INFO(__VA_ARGS__)
#endif
`

并替换 physical_core 内这些高频日志。

5. 构建成功并已安装到手机：

`text
D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
`

6. 安装后已 m force-stop org.nxemu.app.debug 并锁屏。

后续：

- 让用户重新测 Metal Dogs/Mystic Gate，观察去掉高频日志后 FPS/卡顿是否改善。
- Mystic Gate “能进游戏但无法对话”：当前日志显示输入已进 HID，A/B/X/Y ordinal 与 native enum 一致；更像 guest/render/sync 停顿或低速导致，不是简单按键映射反了。需要在去掉高频日志后的包上复测。
- 如果仍慢，下一步继续：驱动对比、减少更多诊断开销、再单独推进 NCE。

记录时间：2026-08-15 16:30:36 +08:00

---

## 2026-08-15 虚拟手柄布局/双摇杆更新

用户反馈：Metal Dogs 已能进正式游戏，但屏幕键位缺少左摇杆，ABXY、+/-/Start/Select、右摇杆整体偏下且按钮太小，容易误触。

本轮修改：

1. Android 前端新增左右两个虚拟摇杆：
   - `L` = Switch left analog stick (`NativeAnalogValues::LStick = 0`)。
   - `R` = Switch right analog stick (`NativeAnalogValues::RStick = 1`)。
   - 手柄摇杆以圆形半透明 View 显示，带死区和发送阈值，避免 move 事件刷太多 JNI。

2. 新增 JNI analog 输入链路：
   - `NativeLibrary.setPlayerAnalog(playerIndex, stickIndex, x, y)`。
   - `Java_org_nxemu_app_NativeLibrary_setPlayerAnalog(...)`。
   - 调用 `OperatingSystem().SetPlayerAnalogState(...)`，最终走 `VirtualGamepad::SetStickPosition(...)`。
   - runtimeStatus 增加 `lastAnalogPlayer/lastAnalogStick/lastAnalogX/lastAnalogY`，方便日志确认。

3. 屏幕布局调整：
   - 保留左侧 D-Pad（ordinal 12/13/14/15），不再把它当摇杆。
   - 新增左摇杆放左下；D-Pad 上移到左摇杆上方。
   - 新增右摇杆放右下但上移，不再贴底。
   - ABXY 放右上侧并上移。
   - `+/-/Start/Select` 中置并上移。
   - 按钮尺寸显著放大：D-Pad 126、ABXY 134、小按钮 174x82、摇杆 260。

4. 诊断日志降噪：
   - analog move 不再每次都写 `/sdcard/ns/logs`，仅释放或每秒最多记录一次，避免玩游戏时移动摇杆造成额外 I/O 卡顿。

验证：

- 构建成功：BUILD SUCCESSFUL in 20s。
- APK 已安装到 realme RMX3700。
- 直接启动 Metal Dogs 10 秒探针：进程保持在 `EmulationActivity`，没有 Java 崩溃。
- analog 探针日志确认：`analog-L`、`analog-R` 都出现 `analog=ok`。
- 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

APK 路径：

```text
D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
````

后续：

- 让用户肉眼确认新版按键位置是否更接近 yuzu/参考项目/其他 Android 模拟器习惯；如遮挡画面，再微调 margin。
- Metal Dogs 速度慢仍主要受 NCE 关闭/Dynarmic 后端限制；NCE 继续独立修，不能污染当前稳定包。
- 日志里 HLE/NVDRV/VI 诊断仍较多，下一步可继续做按需采样/减少默认高频日志。

记录时间：2026-08-15 16:55:00 +08:00

---

## 2026-08-15 手柄补全 + Android 默认降噪/性能路径

本轮状态：

1. 已确认虚拟手柄已补齐用户指出的缺失键：
   - L / R
   - LT(ZL) / RT(ZR)
   - L3 / R3（左右摇杆按下）
   - 左摇杆 L analog 与右摇杆 R analog 也已存在 JNI/native 链路。

2. 最新布局已在真机 5 秒探针中截图确认：
   - 左上：L LT
   - 右上：RT R
   - 左下：左摇杆 + L3
   - 右下：右摇杆 + R3
   - 中下：Select - Start +
   - 右侧：Y X B A
   - D-Pad 保持在左侧。

3. 继续降低 Android 默认高频日志开销：
   - NxemuAndroidDiagnostics::RecordEvent 对热路径类别改为先走 lock-free 原子桶计数，未采样事件不再进入全局 mutex / unordered_map / recent deque / logcat。
   - 扩展热路径类别：NVDRV.nvhost_as_gpu.*、NVDRV.nvhost_ctrl.*、NVDRV.nvmap.*、VI.Manager.*、VI.App.OpenDisplay/OpenLayer 等。
   - Android native 初始化后默认把高频 log class 调到 Error：Service、Service_AM、Service_SM、Service_NVDRV、Service_Nvnflinger、Service_VI、HW_Memory。错误/崩溃仍会输出；默认不再刷 STUBBED/VI/NVDRV warning。

4. 真机快速验证：
   - 已构建并安装 APK。
   - 运行 Metal Dogs 5 秒未崩溃，Activity 保持在 EmulationActivity。
   - 降噪后 5 秒内 logcat 粗计：NxEmuHleDiag 从约 100 降到约 24；YuzuNative Warning 从约 50 降到约 11（后续 Service_AM 也已加入 quiet filter，下一包会更少）。
   - 当前仍是稳定保护包：`cpuBackendActual=Dynarmic`、`nce=0`、`nativeGuard=1`、`frameSkip=0`。
   - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

5. NCE 现状/下一步：
   - 参考源码在 D:\project\switch-emulator-refs，包括 参考项目/yuzu/suyu/sudachi 等。
   - 当前 nxemu 的 NCE 路径已相对 参考项目/yuzu 有大量 Android signal/tpidr/sigaltstack 自定义保护；历史卡点仍是 LoadContext -> SetContext -> RunThread/ReturnToRunCode 这一段。
   - 不能直接打开 NCE 作为默认，避免污染当前 Metal Dogs 可玩稳定包。下一步应做单独实验开关/实验包，只在 adb 控制下打开 NCE，采集 tombstone/logcat 后再合并稳定修复。

记录时间：2026-08-15 17:11:55 +08:00

补充：修正 Service_AM quiet filter 的换行转义问题后，重新构建成功（BUILD SUCCESSFUL in 17s），并已安装最新 APK 到 realme RMX3700；随后执行 `am force-stop org.nxemu.app.debug` 并锁屏。记录时间：2026-08-15 17:14:01 +08:00

---

## 2026-08-15 NCE 参考检索 + 信号入口定位

用户问题：是否可以参考其他项目/GitHub NCE。结论：可以，而且必须参考。优先级：

1. 本地参考源码：
   - `D:\project\switch-emulator-refs\参考项目\src\core\arm\nce\arm_nce.cpp/.s`
   - `D:\project\switch-emulator-refs\参考项目\src\common\signal_chain.*`
   - `D:\project\switch-emulator-refs\skyline\app\src\main\cpp\skyline\nce.cpp`
   - `D:\project\switch-emulator-refs\skyline\app\src\main\cpp\skyline\common\signal.cpp`
   - `D:\project\switch-emulator-refs\strato\app\src\main\cpp\skyline\nce.cpp`
2. 关键参考点：Skyline/Strato 的 `NceTlsRestorer`、`GuestSafeSignalHandler`、进入 guest 前/信号回调内恢复 host TLS 的方案。参考项目/yuzu 的 `Common::SigAction` 直接绕过 ART sigaction wrapper。
3. GitHub API code search 当前匿名请求返回 401，SearxNG/GitHub 网页搜索结果质量一般；本地 refs 已经足够覆盖当前 NCE 关键实现。

本轮修改：

- `src\nxemu-os\core\hle\kernel\physical_core.cpp`
  - 新增 `debug.nxemu.nce.trace` 控制的 Android PhysicalCore NCE trace，默认关闭，adb 调试时开启。
- `src\nxemu-cpu\nce\arm_nce.cpp`
  - 新增实验开关：
    - `debug.nxemu.nce.noinit`
    - `debug.nxemu.nce.norun`
    - `debug.nxemu.nce.control_only`
  - 新增 `NxEmuNCE` 直接 Android log，用于信号入口裸崩时定位。

验证：

- 构建成功，APK 已安装到 realme RMX3700。
- `debug.nxemu.nce.norun=1`：不会崩，能反复到 `RunThread no-run probe active`，说明 CPU core 创建、LoadContext、Initialize、LockThread 都基本可达。
- 正常 NCE（altstack=0）：
  - 已确认链路到：`LoadContext -> SetContext -> SetTpidrro -> SetWatchpointArray -> RunThread -> Initialize -> LockThread -> RunThread entering guest`。
  - `RunThread before/after InstallNceSignalHandlers` 都出现，说明不是 sigaction 安装阶段崩。
  - 随后裸 `SIGSEGV signal 11`，无 debuggerd backtrace。
- 正常 NCE（altstack=1）：
  - 能安装 per-thread signal stack，也到 `RunThread entering guest`。
  - 仍裸 `SIGSEGV signal 11`。

当前判断：

- 之前认为崩在 `SetWatchpointArray` 后/RunThread 前不准确；新增 trace 证明已经进入 guest 前最后阶段。
- 当前真正卡点是 `ReturnToRunCodeByExceptionLevelChange()` 之后：SIGUSR2 ucontext 恢复 guest 或刚进入 guest 后发生 SIGSEGV，但 NCE SIGSEGV handler 没有成功输出 guest fault 日志。
- 下一步应集中对比 Skyline/Strato：把当前 yuzu 式 SIGUSR2/ucontext 进入路径替换或增加一个 Android direct-entry/trampoline 实验路径，避免依赖自信号修改 ucontext；同时继续在 `RestoreGuestContext` / `SaveGuestContext` 内加直接 log 以确认是否成功从 SIGUSR2 handler 返回 guest。

测试后已执行：

````powershell
adb shell am force-stop org.nxemu.app.debug
adb shell input keyevent 26
````

记录时间：2026-08-15 18:28 +08:00

## 2026-08-15 19:12 NCE direct-map 调试更新
- 发现 Android NCE 之前一直在 0x80000000 首条指令取指 SIGSEGV；根因之一是 `HostMemory::EnableDirectMappedAddress()` 后，Linux Impl 的 `AdjustMap()` 仍按高位 fastmem 预留区裁剪，导致低位 guest VA（0x80000000）没有真正 `mmap`。
- 已修改 `src/yuzu_common/host_memory.cpp`：direct-mapped 模式下 `Map/Protect/Unmap` 直接操作 guest VA，跳过 high-fastmem `AdjustMap/free_manager`；测试日志确认 `mmap(0x80000000)` 成功，`mprotect(0x80000000, PROT_READ|PROT_EXEC)` 成功。
- 已修改 `src/nxemu-os/core/hle/kernel/k_process.cpp`：Android NCE 请求时强制把每个 NSO code segment `Protect(Read|Execute)`，不再依赖 patch_size 非零。
- 已修改 `src/nxemu-cpu/nce/arm_nce.cpp`：绕开 IMemory 虚调用诊断，改为 concrete `KProcess::GetCoreMemory()`；并新增 RunThread 等待 TPIDRRO 最多 50ms，避免 Android integration 下 LoadContext/RunThread 竞态。
- 最新构建成功，APK 已安装过；最新未安装的构建位于 `src/android/app/build/outputs/apk/debug/app-debug.apk`（包含 TPIDRRO wait）。
- 最新测试日志目录：`phone-logs/nce-direct-map-fix-20260815-191003`。该版本已经不再卡在未映射 0x80000000；下一步需要安装刚构建的 TPIDRRO wait 版本继续 5 秒探针。
- 注意：当前加入了大量 `NxEmuNCEHost/Heap/Mem` 诊断日志，有预算限制但后续稳定后应降噪/属性开关化。

---

## 2026-08-15 NCE patch-section / post-handler 修复进展
记录时间：2026-08-15 19:53:41 +08:00

本轮继续目标：修 Android NCE，让 Metal Dogs 不再在启动早期崩溃，并继续向可玩 NCE 迈进。

已完成修改：
1. src/nxemu-cpu/nce/patcher.cpp/.h 与 src/core/arm/nce/patcher.cpp/.h
   - 补齐 MRS CNTFRQ_EL0 的 NCE patch：新增 WriteCntfrqHandler()，返回 Common::WallClock::CNTFRQ，避免未 patch 的 CNTFRQ 在 Android user space 触发 SIGILL。
2. src/nxemu-loader/core/loader/nso.cpp（以及同步的 src/core/loader/nso.cpp）
   - 修复 RelocateAndCopy() 写 patch section 前没有扩容 program_image 的问题。
   - 旧行为会把 patch bytes 写到 vector 末尾之外，导致真机跳到 0x8652e16c 时指令为 0x00000000 并 SIGILL。
   - 现在 relocation 前按 patch scratch 扩容，relocate 后 shrink 到精确 image_size。
3. src/nxemu-loader/core/hle/kernel/code_set.h
   - 补齐 PatchSegmentAddr/Offset/Size override。
   - 旧行为 IModuleInfo::PatchSegmentSize() 走默认 0，KProcess 没有给 patch section 做 RX protect。
4. src/nxemu-cpu/patch/patch_collection.cpp
   - 修复 post-SVC trampoline map 只保留最后一个 module 的问题。
   - RelocateAndCopy() 即使还没复制共享 patch section，也会输出当前 module 的 trampolines；现在每个 module 的 CodeSet 都能注册自己的 post handlers。
5. src/nxemu-cpu/nce/arm_nce.cpp
   - Android direct signal log 改为 debug.nxemu.nce.trace 开关控制，并加 256 条预算，避免在信号/高频 SVC 路径里无限 __android_log_print 导致额外崩溃。
   - exec fault mprotect 成功后会在 trace 模式打印当前指令，用于确认是否仍为 zero patch。

验证结果：
- 构建成功，最新 APK：src/android/app/build/outputs/apk/debug/app-debug.apk，时间约 2026-08-15 19:52。
- 真机 5 秒探针日志目录：
  - phone-logs/nce-cntfrq-probe-20260815-194329：确认旧问题，pc=0x8652e16c inst=0x00000000 后 SIGILL。
  - phone-logs/nce-patchprotect-probe-20260815-194907：patch section 已有内容并能进入更多 NCE/SVC 流程，但仍崩。
  - phone-logs/nce-postmap-probe-20260815-195243：rtld 也注册了 post trampolines：base=0x80000000 count=4 total=4；sdk 注册后 total=80。说明 post handler 丢失问题已修复。
- 新现象：不再是 0x8652e16c inst=0 的 SIGILL；NCE 已能反复进入/返回 SupervisorCall (halt=0x4)，PC 到 0x8000148c、0x85b7b660 等位置后，约 0.5~0.6 秒仍出现 signal 11。下一步要定位这个后续 SIGSEGV，重点看 SVC post-resume 是否仍有 PC 未前进/重复调用，或某个 host fault 被 active tid fallback 误判为 guest fault。

下一步建议：
1. 在 ArmNce::RunThread() 增加 post handler hit/miss 低频日志（非信号路径），打印 pc、post_handlers.size()、hit trampoline 地址，确认 0x8000148c / 0x85b7b660 是否命中。
2. 对 ResolveGuestSignalContextAndRestoreHostTls() 加 host-PC 判定：当 candidate 不是 active tpidr 且 signal PC 明显位于 host library 高地址时，不要按 tid fallback 当作 guest fault，先输出 host fault 诊断。
3. 高频 RunThread returned halt=0x4 也需要降噪，否则正常运行时 logcat 过大。
4. 保持默认稳定包仍不强制 NCE；NCE 继续通过 adb property/界面实验开关测试，避免影响 Dynarmic 当前可玩路径。

测试后已执行：
```powershell
adb shell am force-stop org.nxemu.app.debug
adb shell input keyevent 26
```

## 2026-08-15 20:15 NCE post-handler / OwnerProcess 修复
- 本轮按用户要求继续参考 参考项目/yuzu 与 Skyline/Strato：参考项目/yuzu 的 NCE 路径确认仍是 `RunThread -> post_handlers.find(pc) -> ReturnToRunCodeByTrampoline`；Skyline/Strato 对本轮主要启发仍是 Android guest/host TLS/signal 区分。
- 新定位：上一版虽然 `KProcess::LoadModule()` 日志显示已注册 post trampolines（rtld 4、sdk 76，总计 80），但 `ArmNce::RunThread()` 里 `process->GetPostHandlers()` 实际为 0，导致每次 SVC 后都走 `ReturnToRunCodeByExceptionLevelChange`，PC 在 `0x8000148c / 0x85b7b660` 等位置重复，最终容易 signal 11。
- 修复：`src/nxemu-cpu/nce/arm_nce.cpp` 中不再通过 `thread->GetOwnerProcess()` 的接口指针再 `static_cast<KProcess*>`，改为 `static_cast<KThread*>(thread)->GetOwnerKProcess()`；guest fault 路径也同样从 `KThread` 取真实 `KProcess`。修复后 `post_hit=true`，NCE 不再在 0.5s 左右闪退。
- 同步修复：RunThread 退出时同步 `tpidrro_el0`；新增 dispatch/post-hit 诊断，随后将高频 RunThread/LockThread/Initialize/GetContext/SetContext/InvalidateNCE 成功日志做预算/trace 开关降噪，避免 logcat 几秒膨胀到数百 MB。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 已重新构建并安装测试。
- 真机 5 秒探针：`phone-logs/nce-final-smalllog-20260815-201453`。结果：`pidof` 仍返回进程 `25511`，未见 `Fatal signal`/`signal 11`；测试后已 `am force-stop` 并锁屏。
- 当前结论：NCE 已从“启动即崩/0.5s signal 11”推进到“能持续跑过 5 秒探针，不闪退”。下一步应在用户实际玩 Metal Dogs 时确认 FPS/画面，并继续处理性能、花屏/驱动兼容、以及剩余 HLE/NVDRV/VI 日志降噪。

## 2026-08-15 21:25 本轮更新（NCE/加载黑屏修复）

### 用户反馈
- 最新反馈：不闪退，但加载游戏没有成功，FPS 不显示。

### 本轮定位
- 复查上一轮日志 `phone-logs/user-black-no-fps-20260815-210719`，真正卡点不是崩溃，而是 `nxemu-os/core/memory.cpp:980` 高频 `UNIMPLEMENTED()`。
- 该位置是 `Memory::Impl::InvalidateGPUMemory()`，NCE/fastmem 写 GPU/rasterizer 内存时会触发；未实现导致日志刷爆、渲染/加载推进异常。

### 本轮修复
1. 接入 GPU invalidate：
   - `src/nxemu-module-spec/video.h` 增加 `IVideo::InvalidateGPUMemory(const uint8_t*, uint64_t)`。
   - `src/nxemu-video/video_manager.{h,cpp}` 实现：通过 `Host1x.MemoryManager().ApplyOpOnPointer()` 找到设备地址，再调用 `m_gpuCore->InvalidateRegion(address, size)`。
   - `src/nxemu-os/core/memory.cpp` 与 `src/core/memory.cpp` 的 `InvalidateGPUMemory()` 改为调用 `system.GetVideo().InvalidateGPUMemory()`，并按 yuzu/参考项目 逻辑保留 sys_core 互斥保护。
2. 修复/降噪异常路径：
   - `PhysicalCore` 的 breakpoint/prefetch/data-abort/step debug 分支不再 `UNIMPLEMENTED()`；改为输出 backtrace 或挂起线程，避免日志中出现 false-positive “Unimplemented code!”。
   - `core/arm/debug.cpp::SymbolicateBacktrace()` 改为 no-op，保留 raw PC/LR，不再每次 backtrace 触发 `UNIMPLEMENTED()`。
   - `ArmNce::EnsureNceSignalStackForCurrentThread()` 复用 altstack 的高频日志改为只在 `debug.nxemu.nce.trace=1` 时输出。
   - `ArmNce::RunThread()` 的 dispatch/returned/early-halt 高频日志改为受 `debug.nxemu.nce.trace=1` 控制。

### 构建/测试结果
- APK 构建成功：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 已安装到 realme RMX3700 并用 Metal Dogs 做 12 秒探针测试。
- 最终日志目录：`D:\project\_nxemu_src\phone-logs\final-probe-20260815-212353`。
- 结果：
  - 进程未崩溃：`pidof=8721`。
  - `Assertion Failed=0`，`Unimplemented code=0`，`signal 11=0`，`VK_ERROR=0`。
  - FPS/Composite 已恢复显示；截图显示 `Render FPS ~64 / Composite ~64 / Native deferred / NCE 实际开`。
  - overlay 显示 `Load bootGame result ready | 约 11s total`，说明本轮已经从“黑屏无 FPS/加载未成功”推进到“启动可加载并有渲染帧”。
- 测试后已执行：`adb shell am force-stop org.nxemu.app.debug` 和锁屏。

### 下一步建议
1. 继续用真机进入游戏实际内容，验证是否仍有一小段黑屏、对话/输入是否正常。
2. 如果仍慢：继续做 NCE 性能热点采样，优先检查 guest SVC trampoline 循环、GPU driver/分辨率设置、HLE/NVDRV/VI 日志。
3. 现在 `debug.nxemu.nce.trace=0` 默认已大幅降噪；需要 NCE 深度调试时再临时开 `setprop debug.nxemu.nce.trace 1`。

## 2026-08-15 21:50 用户 26.3 驱动黑屏/FPS 不显示排查
- 用户反馈：切到 26.3 后 FPS 不显示、不加载。
- 抓取目录：`phone-logs/user-263-no-fps-20260815-214554`。
- 实际当前驱动：`/data/user/0/org.nxemu.app.debug/files/gpu_drivers/Turnip-v26.3.0-devel.zip`，`gpuCustomDriverName=libvulkan_freedreno.so`，`initializeGpuDriver=ok`。
- 关键结论：不是 ROM 没加载，也不是 Vulkan 初始化失败：`LoadRom accepted` 约 521ms；`VK_ERROR=0`、`signal 11=0`、`Assertion Failed=0`。NCE 后续状态为 `nceEnabled=true / cpuBackendActual=NCE`。
- 问题点：Vulkan 只 present/composite 了 17 帧，之后长期不增长；overlay 因为 present delta=0 显示 `Render FPS -- / Composite --`，画面保持黑屏。日志中有大量 `vk_pipeline_cache.cpp:CreateGraphicsPipeline`，推测 26.3 在此组合下进入“管线/渲染线程停止推进或卡住”的状态。
- 对比：本机 21:23 final-probe 也使用同一个 `Turnip-v26.3.0-devel.zip`，短探针 12 秒内 present/composite 可推进并显示约 64 FPS；说明该问题可能与更长运行、驱动状态、shader cache/pipeline、或某次启动路径有关，不是单纯无法加载 26.3。
- 后续建议：增加 present-stall watchdog（例如 boot ready 后 5~10 秒 presentCount 不增长时输出 `render_stalled`，记录当前 driver、pipeline/shader building、GPU thread状态），并提供一键切回系统/MK-I/上次稳定驱动。必要时清理该驱动的 shader/vk_file_redirect cache 后重测 26.3。
- 本次抓日志后已 force-stop 并锁屏。

## 2026-08-15 22:32:44 +08:00 本轮固定测试/截图/锁屏记忆更新
- 截图固定方法：不要在 PowerShell 中直接 adb exec-out screencap -p > screen.png，容易因文本流/CRLF 导致 PNG 损坏；改用两步：
  `powershell
  adb shell screencap -p /sdcard/ns/logs/nxemu-last-screen.png
  adb pull /sdcard/ns/logs/nxemu-last-screen.png .\screen.png
  `
- 若截图仍无法判断，兜底抓：
  `powershell
  adb shell uiautomator dump /sdcard/ns/logs/window.xml
  adb pull /sdcard/ns/logs/window.xml .\window.xml
  adb shell dumpsys activity top > activity-top.txt
  adb logcat -d -t 20000 -v threadtime > logcat-tail.txt
  `
- 执行任何手机测试/截图/输入命令前先判断锁屏/前台：adb shell dumpsys window policy 或 uiautomator dump。若锁屏，先解锁：adb shell input keyevent 224、上滑、adb shell input text 570765、adb shell input keyevent 66。
- 执行完真机测试必须省电收尾：
  `powershell
  adb shell am force-stop org.nxemu.app.debug
  adb shell input keyevent 26
  `
- 本轮针对用户“三次运行仍卡住”：已确认最新几次不是截图问题，进程有时已被 force-stop；session 日志显示 NCE 下 present/composite 卡死在 153 或 194，Dynarmic 对照能继续推进到 319+，核心仍指向 NCE guest 执行/等待路径。
- 已修改 EmulationActivity.kt：rrenderStall 检测触发时自动调用 NativeLibrary.requestGuestCpuSample()，并在 750ms 后追加一次 delayed runtime/perf snapshot 到 /sdcard/ns/logs/nxemu-session-*.txt，方便回退/黑屏太快时也能看到 OS.Thread/CPU.Core/SVC 最近状态。

## 2026-08-15 22:49:27 +08:00 本轮 NCE 卡住排查与固定流程（纠正版）
- 固定截图方法：不要用 PowerShell 直接重定向 adb exec-out screencap。使用：
  1. adb shell screencap -p /sdcard/ns/logs/nxemu-last-screen.png
  2. adb pull /sdcard/ns/logs/nxemu-last-screen.png .\screen.png
- 截图兜底：抓 window.xml、activity-top、logcat：
  1. adb shell uiautomator dump /sdcard/ns/logs/window.xml
  2. adb pull /sdcard/ns/logs/window.xml .\window.xml
  3. adb shell dumpsys activity top > activity-top.txt
  4. adb logcat -d -t 20000 -v threadtime > logcat-tail.txt
- 每次真机测试前先检查是否锁屏/前台；若锁屏，解锁流程：input keyevent 224 -> swipe 620 2100 620 800 -> input text 570765 -> input keyevent 66 -> wm dismiss-keyguard。
- 每次真机测试结束必须执行：adb shell am force-stop org.nxemu.app.debug，然后 adb shell input keyevent 26 锁屏。
- 本轮修复：EmulationActivity.kt 的 rrenderStall 检测现在会自动 requestGuestCpuSample，并延迟 750ms 再写一次 runtime/perf snapshot 到 /sdcard/ns/logs/nxemu-session-*.txt。
- 本轮修复：nxemu_android_jni.cpp 的 requestGuestCpuSample 不再永久开启 androidFullTrace；输出 androidFullTrace=disabled，避免 SVC/VI/NVDRV 热路径刷屏影响性能。
- 本轮对比：NCE 下 Metal Dogs 多次停在 P/C=32/32、153/153、194/194 或类似值，Render FPS 变为 --；Dynarmic 同条件能持续推进到 P/C 2000+，因此问题仍集中在 NCE 路径。
- 新关键线索：NCE 卡住时 OS.Thread 多数在 wait=4，CPU.Core halt=4（SupervisorCall），PC 常见 0x85b7b5a4/0x85b7b50c/0x85b7b5dc；HLE 最近事件高频 WaitSynchronization/WaitProcessWideKey，Nvnflinger 常见 CacheFramebuffer status=2（CachedBufferReused）且无新 QueueBuffer/Submit。
- 实验记录：尝试过在 InvalidateNCE rasterizer fault 后强制 RasterizerMarkRegionCached(false)，可以减少重复 InvalidateNCE fault，但导致 P/C 更早停在 1/1，已回滚，不保留。
- 当前手机已安装回滚后的 APK，并已 force-stop + 锁屏。最新 APK 路径仍为 src/android/app/build/outputs/apk/debug/app-debug.apk。

## 2026-08-15 23:06:40 +08:00 本轮：参考项目对照 + SVC ring 诊断增强
- 参考来源：本地 `D:\project\switch-emulator-refs` 下 yuzu-android-gdm、citron、参考项目；同时通过 SearxNG/GitHub 关键词检索 `yuzu android NCE arm_nce.cpp WriteSvcTrampoline`、`参考项目 emulator NCE arm_nce.cpp patcher.cpp`，确认当前可参考主线仍是 yuzu/citron 的 NCE patcher 与 参考项目的 Android NCE 生命周期/split patch。
- 对比结论：
  - yuzu-android-gdm/citron 的 `WriteCntpctHandler()` 与当前 nxemu 基本一致，仍是 `NativeClock + CNTVCT scale`；`CNTFRQ` 在当前 nxemu 额外补了常量返回，避免 Android SIGILL。
  - 参考项目 多了 `c_pre` / `m_patch_instructions_pre` split/pre patch 体系，适配超 128MiB 大模块；Metal Dogs 当前 PC/P/C 卡顿不像单纯 split patch 缺失，但后续真实大游戏要继续移植/验证。
  - 参考项目的 `RunThread()` TLS/native_context 生命周期此前已部分同步到 nxemu；本轮不再改 trampoline 行为，避免引入新崩溃。
- 本轮新增诊断：`src/nxemu-os/core/hle/service/nxemu_android_diagnostics.{h,cpp}` 增加独立 `recent_svc` ring：
  - `svcRingEnabled=true` 默认开启；保存最近 256 条 `SVC.*`/相关热路径诊断，不再被热点采样策略稀释。
  - `Snapshot()` 现在输出 `svcEventCount` 与 `recent_svc`，用于对比 NCE 卡住前最后 N 条 `SVC.Call/SVC.Return/QueryMemoryResult/...`。
  - 构建成功：`src/android/app/build/outputs/apk/debug/app-debug.apk`，时间约 35s。
- 真机探针结果：
  - 成功探针目录：`phone-logs\probe-nce-svcring-20260815-230332`。
  - 画面：FPS/Composite 显示约 60，`NCE 实际开`，P/C 推进到约 477/478，Vulkan present/composite 正常增长。
  - 日志：`recent_svc` 已出现，当前短探针主要记录启动期 `QueryMemory`；说明 ring 生效。
  - 测试后已 `force-stop` 并锁屏。
- 注意：后续 30/35s 探针目录 `probe-nce-svcring-30s-*` / `probe-nce-svcring-35s-*` 不能作为 NCE 结论，因为 Activity 在 `surfaceAlive=false` 阶段立刻 pause，未进入 `bootGame`；截图文件为 0 字节。后续 ADB 启动命令必须先确认屏幕 unlocked/on 且 Activity 前台没有被远程/系统打断。
- 后续建议：
  1. 下一轮优先做一个专用 `adb-start-emulation.ps1`，封装：解锁 -> force-stop -> `am start`（远端 shell 内正确引用含空格路径）-> 等待 `surfaceAlive/bootStarted` -> 再输入 A -> 截图/拉 session -> 省电锁屏，避免重复出现 Activity pause/0字节截图。
  2. 用新 `recent_svc` ring 对比 NCE 卡住与 Dynarmic 正常推进的最后 SVC 序列；如果 NCE 卡住时 ring 停在 `WaitSynchronization/WaitProcessWideKeyAtomic`，再检查返回值/超时/线程唤醒。
  3. 继续评估 参考项目 split/pre patch 是否需要移植，尤其是真实大 NSP/XCI/DNSP 模块超过 128MiB 时。
- 已新增测试脚本：`src/android/scripts/adb-probe-nxemu.ps1`。
  - 默认会解锁手机、可选安装 APK、设置 NCE property、远端 shell 内正确引用含空格 ROM 路径、运行探针、抓截图/session/logcat、最后 force-stop 并锁屏。
  - 示例：`powershell -ExecutionPolicy Bypass -File src/android/scripts/adb-probe-nxemu.ps1 -Install -InitialWaitSec 5 -AfterInputWaitSec 8`。

## 2026-08-15 23:29 +08:00 NCE halt reason bitmask 修复与验证
- 核心发现：`src/nxemu-module-spec/cpu.h` 的 `CpuHaltReason` 原来是顺序枚举，但 NCE patcher/SignalInterrupt 继承 yuzu/参考项目 语义，使用 `ORR` / `fetch_or` 组合 halt reason。
  - 旧值示例：`BreakLoop=3`、`SupervisorCall=4`，组合后 `3 | 4 = 7`，会被误判成当前顺序枚举里的 `PrefetchAbort`。
  - 这会在 NCE 被中断、SVC 与 BreakLoop/PrefetchAbort 组合时造成错误调度/错误异常处理，是 Android NCE 卡住的重要隐患。
- 修复：`CpuHaltReason` 改为 `enum class CpuHaltReason : uint64_t` bit flags：
  - `BreakLoop = 1 << 3`
  - `SupervisorCall = 1 << 4`
  - `SupervisorCallBreakLoop = (1 << 4) | (1 << 3)`
  - `PrefetchAbort = 1 << 6`
  - `PrefetchAbortBreakLoop = (1 << 6) | (1 << 3)`
- 同时回滚了两个无效实验：
  1. NCE 下关闭 `ForceMaximumClocks/FastGPUTime`：更差，P/C 约 150 就停。
  2. NCE 下启用 async shader/GPU/presentation：更差，P/C 约 136 就停。
  - 当前源码保留原先较稳定配置：NCE 下 async GPU 关闭，timing shortcuts 开启。
- 构建/安装：APK 已更新并安装到 realme RMX3700。
- 验证结果：
  - 短探针目录：`phone-logs\probe-script-20260815-232609`，NCE 约 25s，P/C 推进到约 1230/1230，未再停在 623。
  - 长探针目录：`phone-logs\probe-script-20260815-232717`，NCE 约 52s，截图显示 `Render FPS 36.0 / Composite 36.0 / Speed 60% / NCE 实际开 / P/C 2575/2575`，present/composite 仍在增长，没有复现此前 20 秒内 rrenderStall 卡死。
- 结论：NCE 卡死问题有明显改善；当前仍慢，下一步应继续性能优化/进入正式游戏内容验证，而不是继续追 20 秒内硬卡死。
- 测试脚本 `src/android/scripts/adb-probe-nxemu.ps1` 已改为 `int $Nce/$SendA` 参数，避免外层 PowerShell 调用 bool 参数被错误转成字符串。
- 测试后脚本已执行 force-stop 并锁屏。

## 2026-08-16 00:15:16 +08:00 本轮：触控摇杆/全屏/外部性能菜单
- 用户反馈：左摇杆上下反、右摇杆需确认；两个摇杆偏小；跳帧/分辨率/NCE 等希望放到外部菜单；画面希望全屏；Android 系统导航按钮默认隐藏，触摸/划过再显示。
- 修改 src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt：
  - 两个摇杆的 native 输入统一把 y 反向：触摸坐标向下为正，但 Switch guest 期望上为正；日志现在显示 nativeY，便于确认。
  - ANALOG_STICK_SIZE 从 260 放大到 340，视觉/触摸区域更接近大按钮手感。
  - 启用沉浸式全屏：onCreate/onResume/onWindowFocusChanged 隐藏 system bars；触摸/滑过 Surface 或根布局时临时显示 2.5 秒后再隐藏。
  - 底部调试/设置栏默认隐藏，不再占用底部游戏画面；新增左上角半透明“菜单/收起”小按钮，点击后临时显示设置栏，7 秒后自动收起。
  - 触控布局底部 margin 不再预留原 toolbar 高度，使画面更接近全屏。
  - frameSkip 启动时改为读取 AppPreferences.frameSkip()，这样外部菜单保存后下次启动能生效；默认值仍为 0。
- 修改 src/android/app/src/main/java/org/nxemu/app/MainActivity.kt：
  - 首页增加外部性能设置按钮：外部设置：跳帧、外部设置：分辨率、外部设置：NCE。
  - 设置写入 AppPreferences.savePerformance()；提示 NCE/分辨率建议重新启动游戏验证，运行中菜单仅作为临时调试更稳。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，约 30 秒，APK 时间 2026-08-16 00:13:23，大小约 15.45 MB。
- 真机短探针：src/android/scripts/adb-probe-nxemu.ps1 -Install -InitialWaitSec 5 -SendA 0 安装成功并完成截图/日志拉取，脚本最后已 force-stop + 锁屏。
  - 探针目录：phone-logs/probe-script-20260816-001409。
  - 注意：本次 Pull-LatestSession 拉到的 session 文件名显示为旧 DQB 路径，说明当前脚本按 /sdcard/ns/logs 最新 mtime 取日志时可能会拿到非本轮 session；后续需要把 session 拉取逻辑改成按启动时间或包名前台 PID/最新文件创建时间过滤，避免误判。
- 后续待办：
  1. 用户实测确认左右摇杆 Y 轴是否都正确；如果右摇杆也反则本轮已同步修正。
  2. 继续调触控布局：若 340 仍小，可继续到 380；若遮挡画面，则给摇杆/ABXY 增加透明度/自动淡出。
  3. 修 adb-probe-nxemu.ps1 的 session 选择，避免拉到旧日志。
  4. 继续 NCE 性能/HLE 高频日志压制。

## 2026-08-16 00:21:17 +08:00 闪退修复：沉浸式全屏调用时机 NPE
- 用户反馈：程序闪退。
- ADB 抓取目录：phone-logs/crash-20260816-001721。
- 崩溃原因：Java 层 NPE，不是 native/NCE 崩溃。
  - 日志：NullPointerException: Attempt to invoke virtual method 'android.view.WindowInsetsController com.android.internal.policy.DecorView.getWindowInsetsController()' on a null object reference。
  - 栈：EmulationActivity.hideSystemBars(EmulationActivity.kt:598) -> EmulationActivity.onCreate(EmulationActivity.kt:127)。
  - 原因：onCreate() 最开头调用 window.insetsController，此时 DecorView 还没创建。
- 修复：src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt
  - 移除 onCreate 开头的 hideSystemBars()。
  - 改为 setContentView(...) 完成后再调用 hideSystemBars()。
  - Android R+ 隐藏/显示系统栏改用 window.decorView.windowInsetsController，不再直接取 window.insetsController。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，APK 时间 2026-08-16 00:19:48。
- 真机验证：src/android/scripts/adb-probe-nxemu.ps1 -Install -InitialWaitSec 5 -SendA 0。
  - 探针目录：phone-logs/probe-script-20260816-002029。
  - 结果：安装成功，EmulationActivity 正常显示触控 overlay/FPS overlay/菜单按钮，未复现 Java 闪退。
  - 测试后脚本已 force-stop org.nxemu.app.debug 并锁屏。

## 2026-08-16 00:26:01 +08:00 触控布局重叠修复
- 用户反馈：左右摇杆与 DPad/ABXY 重叠，L3/R3 也重叠；Select/-/+/Start 太小且太挤。
- 修改 src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt：
  - 左右摇杆继续保持大尺寸 ANALOG_STICK_SIZE=340，但放到底部低位：left/right stick bottomMargin=44。
  - DPad 上移：bottomMargin=440，避免和左摇杆重叠。
  - ABXY 上移：bottomMargin=470，避免和右摇杆重叠。
  - L3/R3 从摇杆环上移出，放到摇杆外侧：left/right margin 404，bottomMargin=126，尺寸 94 -> 110。
  - 中间 Select/-/Start/+ 放大并拉开：SMALL_BUTTON_WIDTH 174 -> 226，SMALL_BUTTON_HEIGHT 82 -> 104，新增 CENTER_BUTTON_GAP=24。
- 构建成功并安装到手机。
- 真机 5 秒探针：phone-logs/probe-script-20260816-002519。
  - 截图确认：DPad、ABXY、左右摇杆、L3/R3、中间功能键已经明显分离，没有再互相压住。
  - 测试结束脚本已 force-stop 并锁屏。

## 2026-08-16 11:53:46 +08:00 系统导航误触/退出方式/手柄/首页与性能显示
- 用户反馈：按左摇杆和 B 时经常弹出 Android 后退/主页/多任务三键；左上角菜单像是在弹系统三键；画面仍不够全屏；Back 容易误退出；首页要显示最后加载内容；游戏中右上角绿色信息太多；询问是否支持手柄。
- 处理策略：不再让普通游戏触摸触发系统导航栏；保留明确入口，避免用户无法退出/最小化。
- 修改 EmulationActivity.kt：
  - 移除 SurfaceView/root setOnTouchListener -> showSystemBarsTemporarily()，正常按左摇杆/B/ABXY 不再主动显示 Android 三键。
  - 左上角按钮文字 菜单 -> 设置，只展开/收起 NxEmu 自己的工具栏。
  - 工具栏新增：系统栏 按钮，手动显示 Android 系统栏 3.5 秒；最小化 按钮，调用 moveTaskToBack(true)。
  - Back 键改为二次确认：首次 Toast 再按一次返回键退出模拟器，2.2 秒内第二次才 stop/finish。
  - Android P+ 设置 LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES，Android R+ 调 setDecorFitsSystemWindows(false)；styles 增加 cutout/透明系统栏，目标是尽量占满刘海/系统栏区域。
  - 右上角性能 overlay 精简为仅 FPS | Speed | Temp。
  - 初步补实体手柄输入：onKeyDown/onKeyUp 映射 DPad、ABXY、L/R/ZL/ZR、L3/R3、Start/Select；onGenericMotionEvent 映射左右摇杆轴。当前是基础版，后续需用真实蓝牙/USB 手柄确认各品牌 A/B/X/Y 是否需要按布局互换。
- 修改 GpuDriverHelper.kt：新增 summaryText()，输出最近/当前驱动名称、版本、库名、来源。
- 修改 MainActivity.kt：首页 refreshStatus() 顶部增加“最近加载/设置”：lastGameName、lastGame、lastFolder、lastFrameSkip、lastResolution、lastNce，并显示 GpuDriverHelper.summaryText()。
- 构建：成功，APK src/android/app/build/outputs/apk/debug/app-debug.apk，时间 2026-08-16 11:51:16。
- 真机短测：phone-logs/probe-script-20260816-115200。
  - 运行页截图确认：左上角为“设置”；右上角仅 FPS -- | Speed -- | Temp 38.3°C batt；未见 Android 三键自动弹出；日志无 NxEmu Java/native 崩溃。
  - 日志显示窗口 req=2772x1240、cutoutMode=1，说明横屏已请求占用全屏/刘海区域。
  - 测试后已 force-stop 并锁屏。
- 注意：当前手柄支持为新增基础映射，尚未接入手柄 UI 检测/重映射页面；若用户有实体手柄，下一步让用户测试 A/B/X/Y、左右摇杆、扳机是否对应，再做可配置映射。

## 2026-08-16 12:05:45 +08:00 实体手柄 A/B 反向修正
- 用户反馈：摇杆/手柄的 B 和 A 反了，其他暂时没看出问题。
- 修改 src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt：
  - 仅交换实体手柄 KeyEvent.KEYCODE_BUTTON_A/B 映射：
    - KEYCODE_BUTTON_A -> SWITCH_BUTTON_B
    - KEYCODE_BUTTON_B -> SWITCH_BUTTON_A
  - 屏幕触控 overlay 的 A/B 不变，仍保持 createInputButton("A", SWITCH_BUTTON_A)、createInputButton("B", SWITCH_BUTTON_B)。
- 构建成功：APK 时间 2026-08-16 12:04:20。
- 已安装到 realme RMX3700，安装后已 force-stop 并锁屏；未跑长游戏，等待用户用实体手柄验证。

## 2026-08-16 12:19:36 +08:00 项目定位、上游同步策略、法务/路线边界理解
- 用户问题：nxemu 官方是否留了 Android 底座；PC 改 Android 难度；当前补完进度；如何持续同步官方 GitHub 更新；本地 PC 版又有金手指/中文等差异；当前 Android 版到底是走 参考项目/yuzu 路线还是 nxemu 自己路线；如何规避 Switch 官方法务风险。
- 当前判断：
  - nxemu 官方主要留下的是 PC core 与少量 Android/portable 线索，不是完整 Android 产品层。
  - 当前 Android App、JNI、Surface、SAF 文件、驱动选择、触控/手柄、日志诊断、Android 生命周期、NCE Android 稳定性，大部分是我们正在补。
  - PC -> Android 不是简单套壳；难点集中在 Vulkan/ANativeWindow、NCE/signal/host memory、SAF/content:// 大文件、Android 生命周期、Turnip/Adreno 驱动、输入和 HLE 性能。
- 当前完成度估算：
  - Android APK/构建约 80%；JNI/native 初始化约 70%；Surface/Vulkan 约 60%；ROM 扫描/加载约 65%；dxci/dnsp 启动约 45%；自定义 GPU 驱动约 60%；触控 UI 约 70%；实体手柄基础约 35%；日志诊断约 75%；NCE 可运行约 45%；NCE 稳定/性能约 30%；整体达成“可玩 Android 版 dxci/dnsp”目标约 35%。
- 本地 git 状态（2026-08-16 fetch 后）：
  - remotes：origin=https://github.com/N3xoX1/nxemu，kknd222=https://github.com/kknd222/nxemu.git。
  - 当前分支：master，HEAD=6649705 Merge MK8 fixes and add-on loader into master。
  - kknd222/master 已到 73021f0 Merge remote-tracking branch 'upstream/master'，本地落后 18 commit。
  - origin/master 已到 3080978 loader: implement PatchManager::PatchNSO (#147)，本地相对 origin 是分叉：本地 ahead 36、behind 18。
- 后续同步策略建议：
  1. 不直接在当前工作树 git pull，先保留 Android 移植与 PC 特性差异。
  2. 建议新建集成分支：sync/upstream-YYYYMMDD 或 android/rebase-YYYYMMDD。
  3. 每次同步先 git fetch --all --prune --tags，再看 git log --left-right --cherry-pick --oneline HEAD...origin/master 与 HEAD...kknd222/master。
  4. 优先按模块 cherry-pick/移植，不做大 rebase：loader、HLE 服务、GPU/Vulkan、CPU/NCE、PC 特性（金手指/中文/补丁）分别成小批次。
  5. 每批次合并后必须：PC 可编译/Android APK 可编译/真机 5 秒探针/Metal Dogs 短测/日志无崩。
  6. 对冲突高的文件（CMakeLists、loader、nvdrv/vi、nce、video、settings）用“对照移植”而不是机械合并，避免覆盖 Android hook。
  7. 本地 Android 改动应逐步拆成主题 patch：android-app、jni-runtime、android-vulkan-surface、android-driver、android-input、android-nce、android-diagnostics，便于以后重放到新上游。
- 路线定位：
  - 核心目标仍是“nxemu 自己路线”：保留 nxemu PC core、高效渲染/着色器路径、dxci/dnsp loader、PC 版已有金手指/中文/补丁等优势。
  - 参考项目/yuzu 是 Android 平台层和 Switch 模拟器成熟实现的参考：NCE、Android lifecycle、Turnip/Adreno、自定义驱动、输入/UI、SAF、HLE 修法可参考，但不应把项目变成 参考项目/yuzu fork。
  - 合理做法是“nxemu core + Android 平台适配 + 对 参考项目/yuzu/citron 的局部经验移植”。
- 法务/合规边界工程建议：
  - APK 不内置 keys、firmware、商业游戏 ROM、解密工具、title/prod keys、固件包。
  - 不提供下载 ROM/keys/firmware 的入口，不提供绕过 DRM/TPM 的说明或自动化功能。
  - UI 文案强调仅加载用户自备/自有备份内容；测试 ROM 优先用 homebrew/NRO 或可合法再分发样本。
  - 对 .dnsp/.dxci 只作为用户本地文件格式处理；避免在项目内包含转换/解密逻辑、密钥或样例商业内容。
  - 参考 yuzu/参考项目的开源实现时保留许可证合规和来源记录；高风险代码（绕保护措施、密钥处理、分发工具链）不移植到 APK。

## 2026-08-16 12:21:06 +08:00 补充：上游同步、路线边界与法务风险理解
- 本次确认：刚刚关于“官方 Android 底座/PC 改 Android 难度/走 nxemu 还是 yuzu-参考项目 路线/如何同步上游”的理解已写入本交接文档，后续每轮重要变更继续维护。
- 上游同步现状（已执行 git fetch --all --prune --tags）：
  - 当前分支 master，HEAD=6649705。
  - origin/master=3080978，本地相对 origin 为 ahead 36 / behind 18。
  - kknd222/master=73021f0，本地相对 kknd222 为 ahead 0 / behind 18。
- 同步原则：当前工作树包含大量 Android 移植、NCE、HLE、GPU、输入和诊断改动，不应直接 git pull/rebase 覆盖；应新建 sync/upstream-YYYYMMDD 或 android/rebase-YYYYMMDD 集成分支，小批次 cherry-pick/对照移植上游 commit。
- 同步顺序建议：
  1. 先冻结当前可测 APK 状态，必要时打 tag 或创建备份分支。
  2. fetch 后用 git log --left-right --cherry-pick --oneline HEAD...origin/master 与 HEAD...kknd222/master 列出差异。
  3. 按模块合入：loader/补丁/金手指/中文 -> HLE 服务 -> GPU/Vulkan -> CPU/NCE -> Android 适配。
  4. 每合一小批就跑 PC 编译、Android assembleDebug、真机 5 秒探针、Metal Dogs 短测、日志检查。
  5. 冲突高文件采用人工对照移植，不能机械覆盖 Android hook：CMakeLists、settings、loader、nvdrv/vi、video、nce、memory、signal_chain。
- 路线定位：Android 版应继续走“nxemu core 自己路线”，保留 nxemu PC 版 dxci/dnsp、渲染/着色器、金手指、中文等优势；yuzu/参考项目/Citron 主要作为 Android 平台层、NCE、输入、SAF、驱动加载、生命周期和 HLE 兼容性的参考，不把项目直接变成 yuzu/参考项目 fork。
- 法务/分发边界：不是靠内置 keys/firmware/商业 ROM/解密器来“规避风险”；工程上应保持 APK 不内置密钥、固件、商业游戏、解密工具，不提供 ROM/keys 下载入口，不提供自动绕过保护流程。.dnsp/.dxci 仅作为用户本地自备文件格式加载处理；测试样本优先使用 homebrew/NRO 或可合法再分发内容。
- 后续执行要求：上游同步、参考项目移植、NCE 修复都必须记录来源、改动目的、测试结果；每轮完成后更新本文件，方便其他人接手。

## 2026-08-16 12:51:24 +08:00 Android NCE 性能推进：默认启用 NCE/异步 GPU，降低 NCE 热路径日志
- 用户要求：先继续提升 Android 性能，重点做好 NCE；继续参考 yuzu/参考项目/Citron/Skyline/Strato/Kenji-NX 等 Android Switch 模拟器路线。
- 本轮本地参考：D:\project\switch-emulator-refs 已有 yuzu-android-gdm、yuzu-mainline-mirror、参考项目、citron、skyline、strato、kenji-nx-android 等；重点对照了 yuzu/参考项目/Citron 的 NCE/Android native 层和 Skyline/Strato 的 NCE 方向。
- 修改 src/android/native/nxemu_android_jni.cpp：
  - kAndroidNceStabilityGuardDefault true -> false，NCE 不再被 native 层默认硬保护关掉；UI 请求 NCE 时可真正进 NCE，debug.nxemu.nce 仍可作为兼容旧脚本/实验属性。
  - NCE 开启时不再关闭全部异步 GPU 路径；默认开启 async shader、async presentation、async GPU emulation。
  - 新增回退开关：debug.nxemu.disable_async_gpu_with_nce=1 可快速关闭 async GPU emulation 以定位设备/驱动兼容问题。
- 修改 src/nxemu-cpu/nce/arm_nce.cpp：
  - NCE guest access fault handled by InvalidateNCE 的热路径日志预算从 512 降到 16；避免大量 LOG_INFO 拖慢 Android NCE。需要完整 NCE signal/dispatch 诊断时使用 debug.nxemu.nce.trace=1。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 12:51。
- 真机验证：realme RMX3700，已安装最新 APK，测试后已 force-stop 并锁屏。
  - 5 秒探针：phone-logs/probe-script-20260816-124618，启动后 nceEnabled=true、cpuBackendActual=NCE、vulkanPresentCount 有增长。
  - 带 A 短测：phone-logs/probe-script-20260816-124845，nceEnabled=true、cpuBackendActual=NCE，asyncShaders=true、asyncGpu=false、asyncPresentation=true，13.9s 左右 present=585，无崩溃。
  - async GPU 实验：phone-logs/probe-script-20260816-124946，asyncGpu=true，13.9s 左右 present=607，无崩溃。
  - 最终默认 async GPU 版：phone-logs/probe-script-20260816-125105，nceEnabled=true、cpuBackendActual=NCE，13.8s 左右 present=605，无崩溃。
- 当前结论：NCE 已能实际开启，不再只是“请求”；异步 GPU 与 NCE 同开在 RMX3700 短测稳定，present 计数略有提升；下一步应继续做真实游戏内 30~60 秒采样、更多驱动对比、HLE/NVDRV/VI 等高频日志/等待路径优化。
- 仍需注意：当前短测截图仍可能停留在游戏加载/菜单过渡，不代表游戏内稳定帧率；用户手动进入正式游戏后的反馈仍重要。

## 2026-08-16 13:33:18 +08:00 Android shader 兼容性推进：ISBERD O/小尺寸 global load、日志降噪
- 目标：继续提升 Metal Dogs / .dnsp 正式游戏流程稳定性与性能，处理上一轮日志中的 Translate ISBERD: O is not implemented 与 Vulkan pipeline 日志过多。
- 对照参考：本地 D:\project\switch-emulator-refs\参考项目、citron 的 internal_stage_buffer_entry_read.cpp。这些项目已补全 ISBERD 的 index/imm/shift/skew/mode/O 路径，本轮按 nxemu 的 yuzu_ 命名适配移植。
- 修改 src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp：
  - 补全 ISBERD 的 src_reg_num/imm/sz/shift/skew/o/mode 字段解析。
  - 支持 shift=U16/B32、skew、mode=Patch/Prim/Attr。
  - 支持 o 路径；U32/F32 用 LoadGlobal32，U8/U16 先 32-bit 对齐 global load，再用 BitFieldExtract 抽取小尺寸，避免 SPIR-V 后端 LoadGlobalU8/U16 未实现导致 pipeline 编译失败。
- 修改 src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/move_special_register.cpp：
  - 对 SR_DIRECTCBEWRITEADDRESSLOW/HIGH/ENABLE 返回 0 作为占位，并降到 LOG_DEBUG，避免正式游戏流程中 Special register 21/22 高频 warning/critical 干扰日志和性能。
- 修改 src/yuzu_video_core/renderer_vulkan/vk_pipeline_cache.cpp：
  - CreateGraphicsPipeline hash 日志从 LOG_INFO 降到 LOG_DEBUG，解决 Android logcat 里 CreateGraphicsPipeline:612 高频刷屏。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间 2026-08-16 13:32:33，大小约 15.46 MB。
- 真机测试：realme RMX3700 / Android 13，Metal Dogs /sdcard/ns/rom/Metal Dogs [0100A6E01681C000][v0][JP].dnsp，探针目录：
  - phone-logs/probe-script-20260816-132527：验证 ISBERD O 不再报错，游戏可到剧情对话，FPS/Speed 约 60/100%。
  - phone-logs/probe-script-20260816-132801：验证 CreateGraphicsPipeline:612 info count=0；但发现 SPIR-V Instruction is not implemented 来自 ISBERD U8/U16 触发未实现的 LoadGlobalU8/U16。
  - phone-logs/probe-script-20260816-133144：修正小尺寸 global load 后，SPIRV unimpl=0、CreateGraphicsPipeline:612=0、ISBERD 未再出现错误；NCE 仍为 nceEnabled=true、cpuBackendActual=NCE，正式游戏流程大部分采样在 60 FPS / 100% speed，剧情/加载切换时仍会短暂 0~43 FPS 波动。
- 最新手机状态：最终 APK 已安装到手机；测试/安装后已 am force-stop org.nxemu.app.debug 并锁屏。
- 当前新增遗留：
  1. SR_DIRECTCBEWRITE* 目前仍是 0 占位，只是降噪；若后续游戏出现渲染缺失，需要继续研究 direct CBE write 真实语义。
  2. 剧情/场景切换时仍可能出现短暂 render stall/FPS=0，需要继续定位 shader 编译缓存、pipeline build、HLE wait/NVDRV/VI 等路径。
  3. ISBERD 的 U8/U16 当前用 32-bit global load + bit extract 规避 SPIR-V 后端未实现，后续可补真正 EmitLoadGlobalU8/U16 作为更完整方案。


## 2026-08-16 13:55:14 +08:00 自适应探针测试逻辑与日志降噪
- 用户确认：短测试/长测试不应固定机械跑满；短测试的本意是轻触式探针，先看是否闪退/进程退出/关键日志是否出现。若日志能判断进程退出或错误，就优先用日志/进程状态；日志判断不了时再截图辅助；时间根据加载/运行状态动态调整。
- 已把该规则固化到测试习惯：
  1. 启动前先解锁，启动后先 5 秒左右轻触探针。
  2. 优先检查 pidof org.nxemu.app.debug、logcat 关键模式、session 自动日志；若 app 已退出或命中关键错误，立即收集并停止。
  3. 只有没命中关键日志且仍需确认画面阶段时才截图；长测也允许“达到目的/发现特征”就提前结束。
  4. 测试后默认 force-stop 并锁屏。
- 修改 src/android/scripts/adb-probe-nxemu.ps1：
  - 新增 -Adaptive、-MaxWaitSec、-PollSec、-StopOnPattern、-NoFinalStop。
  - 运行期间轮询 app 是否存活；默认关键模式包括 FATAL EXCEPTION、SIGSEGV、signal 11/6、Render.Vulkan <Error>、LoadRom failed、renderStall=detected、ANR in org.nxemu。
  - 每次输出 probe-summary.txt，记录 stopReason/appAlive/pid/inputSequence 等，方便之后判断是正常到时、闪退、还是命中错误。
- 修改 src/android/native/nxemu_android_jni.cpp：
  - RrefreshNceEnabledLocked("status") 不再每秒刷 rrefreshNceEnabled 日志；只有 NCE 状态变化时才记录。非 status 关键阶段仍保留日志。
- 构建：APK 构建成功，最新时间约 2026-08-16 13:53:50，并已安装真机。
- 自适应长测：phone-logs/probe-script-20260816-135227
  - 输入序列进入 Metal Dogs 剧情对话页，stopReason=max-wait reached 75s，appAlive=True，无关键错误，截图显示 FPS 60.1 / Speed 100%。
  - 没有命中 renderStall=detected，也无 Render.Vulkan <Error> / SPIR-V Instruction / ISBERD 错误。
- 自适应短测/轻触：phone-logs/probe-script-20260816-135437
  - stopReason=max-wait reached 20s，appAlive=True。
  - rrefreshNceEnabled: reason=status count=0，关键错误 count=0。
  - NCE 仍正常：nceEnabled=true、cpuBackendActual=NCE。
- 当前测试策略建议：
  - 轻触短测：-Adaptive -MaxWaitSec 15~25，用于安装后快速判断闪退/启动错误。
  - 进入菜单/标题：按输入序列跑到目标阶段，-Adaptive -MaxWaitSec 30~60。
  - 正式游戏内容/性能采样：输入序列进入正式场景后，-Adaptive -MaxWaitSec 60~120；若命中错误、卡死特征或已经采到目标 FPS/截图，就提前结束。




## 2026-08-16 14:04:08 +08:00 Android 日志降噪与轻触短测验证
- 本轮目标：继续减少 Android 长测/短测中的高频日志，避免 logcat IO 拖慢和掩盖真正错误。
- 最新日志统计发现的主要噪声：
  - Storage buffer failed to track, using global memory fallbacks：短测约 600+ 条，长测约 800+ 条。
  - perfTicker / perfCounters：每秒输出，信息已经在 overlay/session 中存在，没必要刷 logcat。
- 修改 src/yuzu_shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp：
  - Storage buffer failed to track... 从 LOG_WARNING 降到 LOG_DEBUG。
  - Storage buffer tracked without bias... 也从 LOG_WARNING 降到 LOG_DEBUG。
- 修改 src/android/native/nxemu_android_jni.cpp：
  - 去掉 getPerformanceStats() 中每秒 perfCounters present/composite/fb... 的 Android logcat 输出。
- 修改 src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt：
  - 去掉 perfTicker present/composite/runtimePerfLock... 每秒 Log.i 输出；FPS overlay 和 session 采样仍保留。
- 构建：APK 构建成功，最新时间约 2026-08-16 14:01:40，并通过探针安装到 realme RMX3700。
- 自适应短测：phone-logs/probe-script-20260816-140257
  - stopReason=max-wait reached 21s，appAlive=True，无闪退。
  - 截图显示 Metal Dogs 标题页，右上角 FPS 58.0 / Speed 97% / Temp 33.6°C。
  - 降噪计数：
    - Storage buffer failed to track=0
    - perfTicker=0
    - perfCounters=0
    - refreshNceEnabled: reason=status=0
    - Render.Vulkan <Error>=0
    - SPIR-V Instruction=0
    - ISBERD=0
    - Special register=0
    - SIGSEGV=0
    - FATAL EXCEPTION=0
  - 简单统计里的 ANR=1 已确认是 ColorOS/Oplus HANS 系统日志误伤，不是 ANR in org.nxemu。
- 当前结论：logcat 主要噪声已明显降低；后续更适合做 60~120 秒正式游戏内容采样，重点看真实 FPS 波动、渲染缺失和 HLE/NVDRV/VI wait，而不是被 shader warning/perf ticker 淹没。
- 测试后已由探针脚本 force-stop 并锁屏。


## 2026-08-16 14:17:04 +08:00 输入日志降噪 + 间隔截图探针 + Metal Dogs 长测
- 本轮改动：
  1. `src/android/native/nxemu_android_jni.cpp`：`setPlayerButton` 不再把每次按键写入 Android logcat；按键结果仍保留在 `g_last_status`、runtimeStatus/session 诊断里，避免自动输入长测时 logcat 被 `setPlayerButton:` 淹没。
  2. `src/android/scripts/adb-probe-nxemu.ps1`：新增 `-ScreenshotIntervalSec`、`-ScreenshotMaxCount`，自适应等待期间按间隔输出 `screen-001-tXXXXs.png` 等阶段截图；最终仍输出 `screen.png`。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，时间约 2026-08-16 14:12:25，大小约 15.46 MB。
- 短测目录：`phone-logs/probe-script-20260816-141241`
  - 命令参数：`-Install -InitialWaitSec 5 -SendA 1 -AfterInputWaitSec 12 -Adaptive -MaxWaitSec 24 -PollSec 2 -ScreenshotIntervalSec 8 -ScreenshotMaxCount 4`
  - 结果：appAlive=True，stopReason=max-wait reached 24s；最终截图停在 Metal Dogs 标题页，overlay 显示约 `FPS 59.6 / Speed 99% / Temp 34.5°C`。
  - 关键计数：`setPlayerButton:=0`、`Render.Vulkan <Error>=0`、`SPIR-V Instruction=0`、`ISBERD=0`、`Special register=0`、`SIGSEGV=0`、`FATAL EXCEPTION=0`、`ANR in org.nxemu=0`、`renderStall=detected=0`、`Storage buffer failed to track=0`、`perfTicker=0`、`perfCounters=0`。
  - NCE：启动前部分状态仍可能显示 Dynarmic/false；正式 boot 后 `nceEnabled=true`、`cpuBackendActual=NCE`。
- 长测目录：`phone-logs/probe-script-20260816-141338`
  - 命令参数：`-InputSequence 'B@20:900,LSDOWN@12:800,LSDOWN@2:800,A@2:900,A@8:900' -Adaptive -MaxWaitSec 90 -PollSec 2 -ScreenshotIntervalSec 12 -ScreenshotMaxCount 8`
  - 阶段截图拼图：`phone-logs/probe-script-20260816-141338/contact-sheet.png`。
  - 截图确认流程：标题页 → 加载黑屏/过渡 → 剧情对话 → 正式游戏场景。
  - 结果：appAlive=True，未崩溃；正式游戏场景内最终截图显示约 `FPS 59.3 / Speed 99% / Temp 36.0°C`。
  - 关键计数：`setPlayerButton:=0`、`Render.Vulkan <Error>=0`、`SPIR-V Instruction=0`、`ISBERD=0`、`Special register=0`、`SIGSEGV=0`、`FATAL EXCEPTION=0`、`ANR in org.nxemu=0`、`renderStall=detected=0`、`Storage buffer failed to track=0`、`perfTicker=0`、`perfCounters=0`；`guest access fault=16` 仍是启动期预算日志，当前未观察到崩溃/持续性能异常。
  - 性能采样：后半段 session 多数 `derivedPresentFps/systemFps/gameFps` 在 58.6～60.4，`speedPercent` 约 97.7～100.6。
- 最新结论：Metal Dogs `.dnsp` 在当前 APK 上可进入正式游戏内容，NCE 实际开启，当前更像“可运行 + 需继续优化/兼容性完善”状态，而不是启动崩溃状态。
- 仍需继续：
  1. 研究 `guest access fault=16` 是否只是 NCE 启动期正常探测/修补，还是可进一步减少。
  2. 继续改善触控布局：右摇杆/手柄 logo 仍遮挡画面，后续应做可隐藏/可调透明度/可拖动或按其他模拟器布局优化。
  3. 增加更多游戏 `.dnsp/.dxci/.xci/.nsp` 兼容性测试，区分本体/更新/DLC。
  4. 继续对照 参考项目/yuzu/Citron/Skyline/Strato 的 Android NCE、GPU driver、HLE/NVDRV/VI 路径，提升兼容性与极端场景性能。
- 手机状态：本轮探针脚本测试结束后已 `am force-stop org.nxemu.app.debug` 并锁屏。

## 2026-08-16 14:54:00 +08:00 Star Allies 测试、30/60FPS显示修正、Stretch黑屏回退
- 用户确认：当前目标优先 `.dnsp/.dxci`，标准 `.nsp` 暂不作为主要测试目标；`.nsp` 仍会走标准 PFS0/NCA 解密/解析链路，当前 Android 侧未补完整。
- Kirby Forgotten Land `.nsp` 测试目录：`phone-logs/probe-script-20260816-142510`
  - 结果：`boot=LoadRom failed`，`loaderType=error`，logcat 有 `fssystem_nca_reader.cpp:operator():110: Assertion Failed!`。
  - 结论：符合“NSP链路未完善”的预期，后续先不把 `.nsp` 失败当作当前主线阻塞。
- Kirby Star Allies `.dnsp` 初测目录：`phone-logs/probe-script-20260816-142551`
  - 结果：可启动，可进入标题/世界地图；NCE 实际开启；无 Vulkan/SPIR-V/ISBERD/FATAL/SIGSEGV 关键错误。
  - 画面：标题和地图有画面，但标题/过渡存在蓝色异常背景，说明该游戏仍有渲染兼容性问题。
  - 用户指出：该游戏可能是 30FPS 满速，不能把 30FPS 固定按 60FPS 算成 50% speed。
- 本轮 FPS/Speed 修正：
  - 修改 `src/android/native/nxemu_android_jni.cpp`：`getPerformanceStats()` 新增 `targetGameFpsAuto`，基于采样启发式推断 30/60FPS 目标；`speedPercent` 用 `当前fps / 推断目标fps`，避免 30FPS 游戏被误报为 50% 速度。
  - 修改 `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`：overlay 显示为 `FPS 当前/目标 | Speed xx% | Temp ...`，例如 `FPS 60.2/60 | Speed 100%`。若后续稳定采到 30FPS 阶段，会显示 `/30` 并按 30 计算 speed。
- 全屏/黑边尝试与回退：
  - 曾尝试把 Android 默认 `NXVideoSetting::AspectRatio` 改成 `Stretch`，并强化 Activity fullscreen flags，目的是去掉上下黑边/让画面铺满。
  - 结果：Kirby Star Allies 出现“有声音、vulkanPresentCount 持续增长、但画面全黑”的回归，测试目录：`phone-logs/probe-script-20260816-143837` 和 `phone-logs/probe-script-20260816-144341`。
  - 已回退：`AspectRatio` 恢复 `R16_9`，Activity fullscreen window 额外 flags 也回退到之前稳定写法，只保留 FPS/Speed 修正。
  - 回退验证：`phone-logs/probe-script-20260816-144926`，Kirby Star Allies 画面恢复，可见中文关卡介绍/地图；overlay 显示 `FPS 60.2/60 | Speed 100%`，无关键崩溃/渲染错误。
  - Metal Dogs 回归：`phone-logs/probe-script-20260816-145221`，标题页正常，overlay 显示 `FPS 60.4/60 | Speed 101%`，无关键错误。
- 当前结论：
  1. 30/60FPS 区分已做基础启发式，避免 30FPS 游戏被误判慢；更准确方案应从 guest timing/perf stats/title profile 获取目标帧率。
  2. 直接改 core `AspectRatio::Stretch` 不是安全的全屏方案，会导致部分游戏有声音黑屏。
  3. 黑边/铺满后续应做成 Android 独立显示策略：保留 core 16:9 输出，再在 Android/renderer present 层做可选的 fit/stretch/crop，而不是改 core aspect ratio。
- 手机状态：本轮探针测试结束后已自动 `am force-stop org.nxemu.app.debug` 并锁屏。

## 2026-08-16 15:05:40 +08:00 参考 Yuzu/参考项目/Citron 完成 Android Stretch 全屏
- 用户要求：参考其他 Android Switch 模拟器的全屏做法，解决画面上下黑边。
- 本轮参考：`D:\project\switch-emulator-refs` 下 Citron/Yuzu Android：
  - Activity 侧主要是 `WindowCompat.setDecorFitsSystemWindows(window, false)` + hide system bars；
  - 画面是否铺满由 renderer aspect ratio 设置控制，`RENDERER_ASPECT_RATIO=5` 表示 `Stretch to window`。
  - EmulationFragment 会根据 aspect ratio 更新 Surface 容器布局；Stretch 情况下不设置固定 16:9 比例。
- 关键结论：之前黑屏不是“Stretch 本身必然有问题”，而是把 Stretch 和额外 Activity fullscreen/window flags 混在一起导致 Surface/显示链路回归。只按参考项目路线改 renderer aspect，不加额外 window flags，可以正常工作。
- 本轮改动：
  - `src/android/native/nxemu_android_jni.cpp`
    - Android 默认 `NXVideoSetting::AspectRatio` 设置为 `AspectRatio::Stretch`（值 4）。
    - `BuildPerformanceStatusLocked()` 输出 `aspectRatio=4`、`aspectRatioLabel=Stretch`。
    - 保留上轮 `targetGameFpsAuto` / `FPS 当前/目标` / 30/60FPS speed 修正。
  - `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
    - 保持上一轮回退后的稳定 Activity fullscreen 逻辑，不再加入会导致黑屏的额外 `FLAG_FULLSCREEN`/transparent bar 强化逻辑。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，最新时间约 2026-08-16 14:59:35。
- 真机验证 1：Kirby Star Allies `.dnsp`
  - 目录：`phone-logs/probe-script-20260816-150034`
  - 结果：画面正常，无上下黑边，截图显示已铺满手机宽屏；无黑屏回归。
  - 关键计数：`LoadRom failed=0`、`Render.Vulkan <Error>=0`、`SPIR-V Instruction=0`、`SIGSEGV=0`、`FATAL EXCEPTION=0`、`ANR in org.nxemu=0`、`renderStall=detected=0`。
  - 状态：`aspectRatio=4`、`aspectRatioLabel=Stretch`、`nceEnabled=true`、`cpuBackendActual=NCE`、overlay 约 `FPS 58.8/60 | Speed 98%`。
- 真机验证 2：Metal Dogs `.dnsp`
  - 目录：`phone-logs/probe-script-20260816-150326`
  - 结果：标题页正常，无上下黑边，画面铺满；无黑屏回归。
  - 关键计数：`LoadRom failed=0`、`Render.Vulkan <Error>=0`、`SPIR-V Instruction=0`、`SIGSEGV=0`、`FATAL EXCEPTION=0`、`ANR in org.nxemu=0`、`renderStall=detected=0`。
  - 状态：`aspectRatio=4`、`aspectRatioLabel=Stretch`、`nceEnabled=true`、`cpuBackendActual=NCE`、overlay 约 `FPS 60.1/60 | Speed 100%`。
- 当前结论：Android 版默认已实现参考 Yuzu/参考项目/Citron 的 `Stretch to window` 全屏铺满路线。该方式会横向/纵向拉伸 16:9 游戏来适配手机宽屏，视觉比例可能有轻微变形；后续可做 UI 选项：`原比例16:9 / 拉伸全屏 / 裁切填满`。
- 手机状态：探针结束后已自动 `am force-stop org.nxemu.app.debug` 并锁屏。

## 2026-08-16 15:13:30 +08:00 复核参考项目全屏实现，新增画面比例/拉伸全屏可切换设置
- 用户要求：继续看其他项目怎么做全屏，并照着做。
- 本轮复核本地参考源码：
  - `D:\project\switch-emulator-refs\citron\src\android\...\EmulationActivity.kt`：`WindowCompat.setDecorFitsSystemWindows(window, false)` + `WindowInsetsControllerCompat.hide(systemBars)`；`RENDERER_ASPECT_RATIO=5` 时 `Stretch to window`，Surface 不设固定比例。
  - `D:\project\switch-emulator-refs\参考项目\src\android\...\EmulationActivity.kt` / `EmulationFragment.kt`：同样 Activity immersive，画面比例由 renderer aspect ratio 和 Surface 布局决定。
  - `D:\project\switch-emulator-refs\yuzu-android-gdm\...`：同样的 immersive 系统栏隐藏路线。
  - `D:\project\switch-emulator-refs\skyline\app\...\EmulationActivity.kt`：额外设置 `layoutInDisplayCutoutMode=SHORT_EDGES`，并隐藏 system bars。
- 本轮结论：当前 nxemu Android 已具备参考项目的 Activity fullscreen/immersive 基础；真正消除黑边的关键是 renderer aspect ratio 使用 Stretch，且不要再叠加会导致黑屏的额外 window flag 试验。
- 本轮改动：
  1. `src/android/app/src/main/java/org/nxemu/app/AppPreferences.kt`
     - 新增持久化 `aspectRatio`，默认值 4（`Stretch`）。
     - `savePerformance()` 扩展为保存 `frameSkip/resolutionSetup/aspectRatio/preferNce`。
  2. `src/android/app/src/main/java/org/nxemu/app/NativeLibrary.kt`
     - `setPerformanceProfile()` 增加 `aspectRatio` 参数。
  3. `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
     - 运行页顶部设置栏新增“画面 拉伸全屏/16:9/21:9/16:10/4:3”循环按钮。
     - 默认仍为“拉伸全屏”，可在游戏内临时切换；设置会持久化。
     - 诊断、session log、首页状态增加 aspect 显示，便于确认当前全屏模式。
  4. `src/android/app/src/main/java/org/nxemu/app/MainActivity.kt`
     - 首页外部设置新增“画面”按钮，可在启动游戏前预设画面比例。
  5. `src/android/native/nxemu_android_jni.cpp`
     - 新增 `g_android_aspect_ratio`，`ApplyAndroidPerformanceSettingsLocked()` 不再硬编码 Stretch，而是使用 Java 传入/持久化的 aspect ratio。
     - `BuildPerformanceStatusLocked()` 输出真实 `aspectRatio` 与 `aspectRatioLabel`；修复 runtime 初始化前短暂误报默认 16:9 的诊断问题。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，最新时间约 2026-08-16 15:12:32，大小约 15.46 MB。
- 真机轻触探针：
  - 设备：realme RMX3700 / Android 13 / serial=b72182d。
  - 游戏：`/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp`。
  - session：`/sdcard/ns/logs/nxemu-session-20260816-151300-866-KirbyStar_Allies_v4.0.dnsp.txt`。
  - 结果：安装成功，5 秒内 app 存活，`boot=LoadRom accepted`。
  - 关键验证：从 activity-create 到 runtime/boot 阶段均显示 `aspectRatio=4`、`aspectRatioLabel=Stretch`，未再出现初始化前 16:9 误报。
- 当前使用建议：默认“拉伸全屏”最符合用户想要的铺满手机屏幕；如果后续某游戏出现 Stretch 相关画面异常，可在运行页或首页切到 `16:9` 复测，区分是比例拉伸问题还是 Vulkan/shader/HLE 兼容问题。
- 手机状态：本轮测试结束后已 `am force-stop org.nxemu.app.debug` 并锁屏。

## 2026-08-16 15:19:30 +08:00 剩余项推进：触控隐藏开关 + 探针 session 精准拉取
- 用户要求：查看当前进度、未完成项，并继续做剩下的。
- 当前总体进度粗估：
  - Android APK 工程/构建/安装链路：85%。
  - `.dnsp/.dxci` 基础加载和运行：60%～65%，Metal Dogs/Kirby Star Allies 已可运行，仍需更多游戏兼容验证。
  - NCE：60%，已经实际开启并能跑过短测/部分游戏，仍需长测、卡顿/等待路径和更多 title 验证。
  - 渲染/Vulkan/驱动兼容：45%～55%，Turnip/系统/MK-I 等驱动可用但仍有 Kirby 蓝色异常、过渡黑屏、个别驱动卡住等问题。
  - Android UI/触控/日志：65%～70%，已有首页、运行页、触控、手柄、日志、全屏切换；仍需布局细化、裁切填满、更多设置项菜单化。
  - 标准 NSP/keys/firmware 链路：低优先级，当前不作为主线；`.nsp` 仍可能失败。
- 本轮完成 1：触控遮挡优化
  - 修改 `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`。
  - 运行页顶部设置栏新增“隐藏触控/显示触控”按钮。
  - 可隐藏/恢复：左右摇杆、L3/R3、DPad、ABXY、L/R/LT/RT、Select/Start/+/-。
  - 隐藏后仍保留左上角“设置”、顶部工具栏、停止/最小化/系统栏等入口，不会把用户困在游戏里。
  - session log 记录 `touch-controls-toolbar-button visible=...`，方便确认用户是否手动隐藏过触控层。
- 本轮完成 2：adb 探针 session 选择修复
  - 修改 `src/android/scripts/adb-probe-nxemu.ps1`。
  - 启动前写入 `/sdcard/ns/logs/nxemu-probe-start.marker`。
  - 拉取 session 时优先使用：`find /sdcard/ns/logs -name 'nxemu-session-*.txt' -newer marker | sort | tail -n 1`。
  - 输出 `session-source.txt`，标记 `sessionSource=marker-filtered ...` 或 fallback。
  - 避免之前按 `ls -t` 误拉到旧 session 的问题。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，最新时间约 2026-08-16 15:17:11，大小约 15.46 MB。
- 真机短测：
  - 设备：realme RMX3700 / Android 13。
  - 探针目录：`phone-logs/probe-script-20260816-151904`。
  - 结果：appAlive=True，`bootGame result ready`，`vulkanPresentCount=63`，`cpuBackendActual=NCE`。
  - session 拉取验证：`sessionSource=marker-filtered /sdcard/ns/logs/nxemu-session-20260816-151915-611-Metal_Dogs__0100A6E01681C000__v0__JP_.dnsp.txt`。
- 当前仍未完成/下一步优先级：
  1. 性能：继续做 60～120 秒正式游戏内容采样，区分 CPU/NCE、Vulkan shader/pipeline、HLE wait 哪个是瓶颈。
  2. 渲染兼容：继续查 Kirby Star Allies 蓝色异常背景/过渡黑屏，优先对照 参考项目/Citron/Yuzu shader 和 Vulkan 修法。
  3. 驱动兼容：对比系统驱动、MK-I、Turnip 26.3/26.2/26.1 参考项目-fix/v849，记录“是否花屏/是否能继续/FPS/Speed”。
  4. 触控体验：隐藏开关已完成，后续可做可拖动/布局配置/自动淡出。
  5. 显示模式：当前有拉伸全屏/多比例切换，后续若要“裁切填满”需继续改 renderer/present layout，不能只改 Activity flags。
  6. NSP 标准链路：暂低优先级；当前主线仍是 `.dnsp/.dxci`。
- 手机状态：本轮测试结束后脚本已 `am force-stop org.nxemu.app.debug` 并锁屏。

## 2026-08-16 15:25:30 +08:00 进度更新 + Kirby 黑色方块排查工具
- 用户反馈：Kirby 画面有些黑色方块，询问是驱动问题还是缓存问题，并要求继续未完成项。
- 基于当前日志/测试的判断：
  - 现有 Kirby 相关 session 中没有看到 `VK_ERROR`、`FATAL EXCEPTION`、`SIGSEGV` 或明确 SPIR-V 编译崩溃；`vulkanPresentCount` 持续增长，说明不是 Vulkan 完全不可用。
  - 当前使用/恢复的自定义驱动显示为 `turnip_mrpurple_T25-raw.adpkg.zip`，运行库 `vulkan.purple.so`。
  - 因此 Kirby 黑色方块更像“驱动 + shader/pipeline cache + renderer 兼容”的组合问题；不能仅凭现象断定是驱动或缓存。
  - 快速判别标准：清理缓存后黑块消失/减少 => 更偏向缓存污染或管线缓存问题；清理后仍存在，并且换系统/MK-I/其他 Turnip 有差异 => 更偏向驱动兼容；所有驱动一致 => 更偏向 nxemu/yuzu_video_core shader/纹理同步 bug。
- 本轮改动 1：新增图形缓存清理入口
  - 修改 `src/android/app/src/main/java/org/nxemu/app/GpuDriverHelper.kt`。
  - 新增 `clearGraphicsCaches()`：清理 adrenotools file redirect、app cache、external cache、常见 shader/pipeline/vulkan/gpu cache 目录。
  - 清理结果输出每个目录的 `existed/deleted/before/after/path`，便于判断是否真有缓存被清掉。
  - `statusText()` 新增 `fileRedirectSize=... bytes`。
  - 修改 `src/android/app/src/main/java/org/nxemu/app/MainActivity.kt`：首页新增“清理图形/驱动缓存”按钮。
- 本轮改动 2：新增“图形兼容模式”
  - 修改 `AppPreferences.kt / NativeLibrary.kt / MainActivity.kt / EmulationActivity.kt / nxemu_android_jni.cpp`。
  - 首页和运行页均新增“图形兼容 开/关”。
  - 默认关闭，性能优先。
  - 开启后 native 设置：
    - `UseVulkanPipelineCache=false`，关闭 Vulkan 驱动管线缓存；
    - `UseAsynchronousShaderBuilding=false`，关闭异步 shader；
    - `EnableReactiveFlushing=true`，开启更保守的 GPU 内存刷新。
  - 目的：专门用于 Kirby 黑色方块/花屏/贴图异常复测。开启后可能更慢，但能帮助区分缓存/异步/同步问题。
  - 诊断输出新增：`graphicsCompat`、`vulkanPipelineCache`、`reactiveFlushing`。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，最新时间约 2026-08-16 15:24:31，大小约 15.47 MB。
- 真机验证：
  - 已安装最新版 APK 到 realme RMX3700。
  - 启动 `MainActivity` 3 秒，进程存活，无 `org.nxemu` 相关 `FATAL EXCEPTION`。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 建议用户测试 Kirby 黑方块的顺序：
  1. 首页点“清理图形/驱动缓存”；
  2. 保持当前驱动，直接启动 Kirby 复测；
  3. 如果仍有黑色方块，首页打开“图形兼容模式”，重新启动 Kirby；
  4. 如果兼容模式仍有黑块，再分别切系统驱动/MK-I/其他 Turnip 对比。
- 当前进度更新：
  - Android APK 工程/构建/安装链路：约 87%。
  - `.dnsp/.dxci` 基础加载和运行：约 62%～67%。
  - NCE：约 60%～62%，已实际开启，仍需更长游戏内采样。
  - 渲染/Vulkan/驱动兼容：约 48%～55%，新增缓存清理和图形兼容模式后排查效率提升，但 Kirby 黑方块仍需用户复测或更长 ADB 截图对比。
  - Android UI/触控/日志：约 70%，已有隐藏触控、画面比例、图形兼容、清缓存、自动日志、session 精准拉取。
- 下一步：根据 Kirby 复测结果决定方向：若清缓存/兼容模式有效，则继续细化缓存/同步策略；若无效，则对照 参考项目/Citron/Yuzu 的 shader/texture cache 和 Vulkan rasterizer 修 Kirby 相关渲染缺陷。

## 2026-08-16 15:33:00 +08:00 参考项目 风格 UI 第一阶段：全外壳 + 运行页
- 用户要求：UI 不光运行页，整个外壳也要照 参考项目 来。
- 本轮参考：`D:\project\switch-emulator-refs\参考项目\src\android\app\src\main\res\layout\fragment_emulation.xml`。
  - 参考项目 运行页结构：全屏 Surface、InputOverlay、右上 stats overlay、左侧 in-game menu、右侧 quick settings sheet。
  - 参考项目 外壳整体风格：深色 Material 风格、卡片/分组、主要操作突出、设置和状态分层。
- 当前工程暂不引入 AndroidX DrawerLayout/Material 依赖，避免大范围 Gradle/资源迁移；先做“无依赖等价版”。
- 本轮运行页改动：`src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
  - 旧版顶部横向工具栏改为 参考风格 两侧抽屉：
    - 左上角 `☰` 打开左侧 `NxEmu Menu`：停止、最小化、系统栏、诊断、日志、采样、NextLoad。
    - 右上角 `⚙` 打开右侧 `Quick Settings`：跳帧、分辨率、画面比例、图形兼容、NCE、隐藏触控。
  - FPS/Speed/Temp overlay 改到顶部居中，减少与左右入口冲突。
  - 保持全屏 Surface + 触控 overlay；菜单自动隐藏，避免长期遮挡画面。
- 本轮首页/外壳改动：`src/android/app/src/main/java/org/nxemu/app/MainActivity.kt`
  - 首页由原始竖向按钮列表改成 参考风格 深色 shell：
    - 顶部 Hero：`NxEmu Android` + `参考风格 shell · nxemu core · DNSP/DXCI focused`。
    - `Library` 卡片：运行已选择、运行上次、选择文件、授权目录、扫描 `/sdcard/ns/rom`。
    - `Graphics` 卡片：自动安装驱动、选择驱动 ZIP、授权驱动目录、恢复系统驱动、清理图形缓存。
    - `Performance` 卡片：跳帧、分辨率、画面比例、图形兼容、NCE。
    - `Diagnostics` 卡片：授权所有文件、自动日志、复制日志、hbmenu 测试。
    - `Status / Diagnostics` 独立日志卡片。
  - 按钮和卡片统一深色圆角样式，主启动按钮高亮。
- 主题改动：`src/android/app/src/main/res/values/styles.xml`
  - `Theme.Material.Light.NoActionBar` -> `Theme.Material.NoActionBar`。
  - 增加暗色 window background / accent。
- 构建：`src/android/app/build/outputs/apk/debug/app-debug.apk` 构建成功，最新时间约 2026-08-16 15:30:38，大小约 15.47 MB。
- 真机验证：
  - 已安装到 realme RMX3700。
  - 首页启动 3 秒进程存活，无 `org.nxemu` 相关 `FATAL EXCEPTION`。
  - 运行页用 Metal Dogs 短启 4 秒进程存活，无 UI 构造崩溃；Vulkan pipeline cache 日志正常出现。
  - 截图保存：`phone-logs/ui-参考项目-shell-20260816-1531/main.png`、`phone-logs/ui-参考项目-shell-20260816-1531/run.png`。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前 UI 进度：
  - 参考项目 风格第一阶段完成：约 70%～75%。
  - 未完成：真正 Material/DrawerLayout 依赖化、游戏封面网格/列表、可拖动触控布局、触控配置保存、更多设置页细分、裁切填满显示模式。
- 下一步 UI 方向：
  1. 首页增加游戏列表/卡片，而不是只靠状态文本列扫描结果。
  2. 运行页触控按键做自动淡出/长按编辑/拖动保存。
  3. 若允许引入 AndroidX/Material，再把无依赖侧栏替换为真正 DrawerLayout + NavigationView，更接近 参考项目 原版。

## 2026-08-16 15:42:41 +08:00 参考项目 风格 UI 第二阶段：首页游戏库卡片
- 用户最新要求：继续把 Android 外壳整体向 参考项目 风格靠拢，不只是运行页。
- 本轮改动：src/android/app/src/main/java/org/nxemu/app/MainActivity.kt
  - 首页新增 Game Library 卡片区，不再只把扫描结果塞进诊断文本。
  - 扫描 /sdcard/ns/rom 或 SAF 授权目录后，会把最多 24 个 .dnsp/.dxci/.nro/.nsp/.xci 渲染成可点击卡片。
  - 卡片显示：序号、文件名、扩展名、文件大小、本体概率高/可能更新DLC/预处理格式/homebrew 等判断、真实 URI/路径。
  - 点击卡片：设置为当前选择并保存 lastGame；长按卡片：直接启动该游戏。
  - Hero 顶部新增“当前选择”摘要，直接显示当前游戏名和路径。
  - 修复了一个偏好脏数据问题：lastGamePath 已经切到 Metal Dogs，但 lastGameName 仍显示 Kirby 的错配；现在普通文件路径会优先从真实路径推导游戏名并回写偏好。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 15:41:48，大小约 15.47 MB。
- 真机短测：
  - 已安装到 realme RMX3700。
  - 首页启动 4 秒进程存活，无 org.nxemu 相关 FATAL EXCEPTION。
  - 截图保存：phone-logs/ui-library-20260816-1542/main.png。
  - 截图确认：Hero 当前选择已正确显示 Metal Dogs [0100A6E01681C000][v0][JP].dnsp，不再错显示 Kirby。
  - 测试后已 am force-stop org.nxemu.app.debug 并锁屏。
- 当前 UI 进度：参考风格 外壳约 78%～82%。未完成：真正封面网格/标题ID图标、搜索过滤、Material/DrawerLayout 依赖化、触控布局编辑保存、运行页设置页更细分。
- 下一步建议：继续做游戏库搜索/过滤和封面占位；或回到主线性能/NCE/渲染兼容，针对 Kirby 黑块和 Metal Dogs 帧率做长测。


## 2026-08-16 15:50:48 +08:00 参考项目 风格 UI 第二阶段补充：游戏库搜索/过滤 + 卡片封面占位
- 本轮继续完善 src/android/app/src/main/java/org/nxemu/app/MainActivity.kt。
- 新增首页游戏库搜索框：
  - 支持按文件名、路径、扩展名、分类提示过滤；例如 metal、kirby、dnsp、dxci、v0。
  - 搜索结果会显示 显示 N/总数，无匹配时给出清空/换关键词提示。
- 游戏卡片从纯文本改成 参考风格 横向卡片：
  - 左侧有格式封面占位块，显示 DNSP/DXCI/NRO 等扩展名。
  - 右侧显示游戏名、格式、大小、本体/更新DLC判断、真实路径。
  - 点击卡片选择游戏并保存 lastGame；长按卡片直接启动。
- 文案修正：明确“长按卡片直接启动”，不再写“双击”。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 15:49:54，大小约 15.48 MB。
- 真机短测：
  - 已安装到 realme RMX3700。
  - 首页启动 3 秒进程存活，无 org.nxemu 相关 FATAL EXCEPTION。
  - 中间一次坐标轻触误点到系统 DocumentsUI 的驱动目录选择器，已退出/force-stop 后重新只做首页启动测试。
  - 测试后已 am force-stop org.nxemu.app.debug 并锁屏。
- 当前 UI 进度：参考风格 外壳约 82%～85%。
- 下一步可选：
  1. 继续 UI：游戏库封面网格/搜索按钮/标题ID封面缓存；
  2. 回主线：NCE 长测、Metal Dogs/Kirby 性能与渲染兼容、Kirby 黑块驱动/缓存对比。


## 2026-08-16 15:57:02 +08:00 参考风格 外壳收尾阶段：诊断折叠 + 双列游戏库
- 用户要求：先完成外壳，再继续 NCE、星河/卡比黑块、驱动兼容。
- 本轮继续只改 Android 外壳，未动 NCE/渲染核心。
- 修改文件：src/android/app/src/main/java/org/nxemu/app/MainActivity.kt。
- 首页 Hero 新增外壳状态摘要：
  - 显示 NCE 请求状态、跳帧、分辨率、画面比例、日志模式。
  - 用于替代之前必须展开大段诊断才能知道当前配置的问题。
- 诊断区默认折叠：
  - Diagnostics 卡片新增“显示诊断详情/隐藏诊断详情”按钮。
  - 默认隐藏 Status / Diagnostics 大文本，首页更像正常前端外壳，不再一打开就是一堆日志。
  - 复制首页日志仍保留，展开/隐藏不影响诊断文本生成。
- 游戏库继续完善：
  - 搜索/过滤框保留。
  - 列表改为双列 参考风格 网格卡片。
  - 每张卡片左侧是格式封面占位块，右侧是游戏名、大小、类型判断、路径。
  - 点击选择，长按直接启动。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 15:55:05，大小约 15.48 MB。
- 真机短测：
  - 已安装到 realme RMX3700。
  - 首页启动 3 秒进程存活，无 org.nxemu 相关 FATAL EXCEPTION。
  - 截图保存：phone-logs/shell-complete-20260816-1556/home-folded.png、phone-logs/shell-complete-20260816-1556/home-library.png。
  - 截图确认：Hero 配置摘要正常、诊断默认折叠、Game Library 搜索框和空状态正常显示。
  - 测试后已 am force-stop org.nxemu.app.debug 并锁屏。
- 当前外壳进度：约 88%～90%。
- 外壳剩余低优先级项：
  1. 真正封面图片/标题 ID 缓存；
  2. 更完整的设置页分屏，而不是首页按钮式设置；
  3. 引入 AndroidX/Material 后改成真正 DrawerLayout/NavigationView；
  4. 触控布局编辑器和按键布局导入/导出。
- 下一步按用户要求：外壳基本可先收住，下一轮回到主线：NCE 长测/修复、Metal Dogs/Kirby/星河黑块、驱动兼容对比。


## 2026-08-16 16:09:41 +08:00 回主线前置：详细性能 HUD / NCE 与渲染观测增强
- 用户要求：外壳基本完成后继续 NCE、星河/卡比黑块、驱动兼容。本轮先补观测能力，便于后续截图/日志直接判断问题。
- 修改文件：
  - src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt
  - src/android/app/src/main/java/org/nxemu/app/MainActivity.kt
  - src/android/app/src/main/java/org/nxemu/app/AppPreferences.kt
- 新增运行页“性能HUD 精简/详细”切换：
  - 位于右侧 Quick Settings。
  - 精简模式保持原来的 FPS / Speed / Temp，避免遮挡。
  - 详细模式显示多行：FPS/target、Speed、frame time、温度、CPU backend/NCE、跳帧、分辨率、画面比例、驱动名/库名、asyncGpu、asyncShaders、pipeline cache、graphicsCompat、present/composite fps/count、FB 尺寸。
- 新增首页持久化设置：
  - Performance 卡片新增“外部设置：性能HUD 精简/详细”。
  - 写入 AppPreferences.perfHudDetailed，下次启动游戏直接生效。
  - 首页 Hero 摘要也显示 HUD=精简/详细。
- 复制/保存日志新增 perfHudDetailed=...，方便确认当次测试 HUD 状态。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 16:08:13，大小约 15.48 MB。
- 真机探针：realme RMX3700，脚本自动安装/启动/拉日志/锁屏。
  - phone-logs/probe-script-20260816-160540：appAlive=True，boot 后 nceEnabled=true、cpuBackendActual=NCE，vulkanPresentCount 从 69 增到 375。
  - phone-logs/probe-script-20260816-160841：appAlive=True，boot 后 nceEnabled=true、cpuBackendActual=NCE，vulkanPresentCount 从 62 增到 367。
  - 无 FATAL EXCEPTION / SIGSEGV / Render.Vulkan <Error> 命中；测试结束脚本已 force-stop 并锁屏。
- 结论：新增 HUD/持久化设置未破坏 Metal Dogs 短启动；NCE 在 boot 后仍实际开启。下一步可以正式做：
  1. 用详细 HUD 对 Metal Dogs 做 60~120 秒回归，确认性能无退化；
  2. 打开详细 HUD 测星河/卡比黑块，截图直接看驱动、FB、compat/cache/async 状态；
  3. 再按系统驱动/MK-I/Turnip 26.3/当前驱动做驱动兼容矩阵。


## 2026-08-16 16:13:41 +08:00 自动详细 HUD 探针入口
- 追加改动：
  - EmulationActivity.kt 新增 EXTRA_PERF_HUD_DETAILED，adb/脚本可直接启动详细性能 HUD，不必人工进右侧 Quick Settings。
  - src/android/scripts/adb-probe-nxemu.ps1 新增参数 -PerfHudDetailed 1|0|-1：
    - 1：强制详细 HUD；
    - 0：强制精简 HUD；
    - -1：不传 extra，使用 AppPreferences 保存值。
- 构建：src/android/app/build/outputs/apk/debug/app-debug.apk 构建成功，最新时间约 2026-08-16 16:12:02，大小约 15.48 MB。
- 真机短测：phone-logs/probe-script-20260816-161214
  - 命令使用 -PerfHudDetailed 1。
  - appAlive=True，boot 后 nceEnabled=true、cpuBackendActual=NCE，vulkanPresentCount 增到 373。
  - 最终截图 phone-logs/probe-script-20260816-161214/screen.png 确认详细 HUD 生效：显示 CPU NCE nce=true、GPU Turnip驱动 T25/vulkan.purple.so、asyncG=true asyncS=true cache=true compat=false、present/composite fps/count、FB 1920x1080。
  - 测试结束脚本已 force-stop 并锁屏。
- 后续测试建议：所有黑块/花屏/驱动兼容对比都带 -PerfHudDetailed 1，截图本身即可判断驱动、NCE、FB、compat/cache/async 状态。


## 2026-08-16 16:36:00 +08:00 Kirby 深蓝块驱动矩阵与缓存清理修正
- 用户反馈：卡比黑块已消失，但仍有少量深蓝色方块。
- 本轮修复：
  - GpuDriverHelper.clearGraphicsCaches() 增加清理 files/user/shader、files/user/pipeline_cache、files/user/vulkan_cache、files/user/cache。
  - 原先只清 files/shader 等目录，实际 yuzu/nxemu Vulkan pipeline cache 位于 files/user/shader/<titleid>/vulkan_pipelines.bin，可能导致驱动/compat 对比被旧缓存污染。
  - adb-probe-nxemu.ps1 增加参数：-GraphicsCompat、-FrameSkip、-ResolutionSetup、-AspectRatio、-ClearGraphicsCaches，便于自动做渲染兼容矩阵。
  - EmulationActivity 支持通过 intent extra 覆盖 frameSkip/resolution/aspect/graphicsCompat。
- 卡比对比测试（realme RMX3700，NCE=true，frameSkip=0，Res=1/2X，详细 HUD，均清缓存）：
  1. T25 / vulkan.purple.so / compat=false：可进游戏，黑块已消失，用户仍看到少量深蓝块；约 30/30 或 60/60 满速场景。
     - 日志目录：phone-logs/probe-script-20260816-162352
  2. T25 / vulkan.purple.so / compat=true：兼容模式生效（asyncS=false/cache=false/reactiveFlushing=true），仍需用户肉眼确认深蓝块；性能约 29.3/30，Speed 98%。
     - 日志目录：phone-logs/probe-script-20260816-162502
  3. Mesa Turnip v26.3.0-devel / libvulkan_freedreno.so：实际切换成功后黑屏但 FPS 约 58.7/60，不适合作为卡比当前默认驱动。
     - 日志目录：phone-logs/probe-script-20260816-162853
  4. mesa-turnip-main-V26.1-参考项目-fix-latest-crash-fix：标题页大面积蓝色块状花屏，不适合卡比。
     - 日志目录：phone-logs/probe-script-20260816-163021
  5. Biosensor - MK-I Phoenix：标题页大面积蓝色覆盖/花屏，不适合卡比。
     - 日志目录：phone-logs/probe-script-20260816-163210
- 当前结论：
  - 少量深蓝块不是旧 shader/pipeline cache 单独造成。
  - 当前设备/卡比组合下 T25 仍是最好的已测驱动。
  - 26.3、参考项目-fix、MK-I 均比 T25 更差。
  - 下一步如果继续修深蓝块，应进入 renderer 侧：texture cache / shader precision / framebuffer copy / reactive flushing 相关逻辑，对照 yuzu/参考项目/Citron 的对应实现。
- 代码策略变更：GpuDriverHelper.driverPreferenceScore() 改为优先 T25/purple/mrpurple，降低 26.3 分数并惩罚 参考项目-fix/Biosensor/MK-I，避免“自动安装推荐驱动”把用户切到卡比黑屏/蓝屏驱动。
- 手机当前已恢复选择：/sdcard/ns/qudong/turnip_mrpurple_T25-raw.adpkg.zip，并已 force-stop + 锁屏。

## 2026-08-16 16:50:00 +08:00 Kirby 蓝块性质进一步定位：非单纯 ASTC/异步 GPU
- 用户补充：T25 也是先出现大片蓝色块，首页角色走动后，角色走过痕迹处的蓝块会被消去。
- 现象判断：这更像 texture/render-target cache dirty tracking、framebuffer copy/blit、barrier/invalidation 刷新不及时；不像普通 shader cache 坏，也不像单纯驱动不能画。
- 已加调试能力：
  - native 支持 debug.nxemu.astc_mode=0/1/2，对应 CPU/GPU/CPU Async ASTC。
  - 详细 HUD 显示 ASTC 模式。
  - adb-probe-nxemu.ps1 新增 -AstcMode。
- 本轮测试：T25/vulkan.purple.so，NCE=true，Res=1/2X，清缓存：
  - ASTC=1/GPU：蓝块仍在，日志目录 phone-logs/probe-script-20260816-164252。
  - ASTC=2/CPU Async：蓝块仍在，日志目录 phone-logs/probe-script-20260816-164551。
  - debug.nxemu.disable_async_gpu_with_nce=true：asyncG=false，蓝块仍在且速度降到约 77%，日志目录 phone-logs/probe-script-20260816-164725。
- 分辨率说明：Res1_2X 在代码中是 up_scale=1/down_shift=1，即内部渲染 0.5 倍；HUD 的 FB 1920x1080 是最终合成/输出 framebuffer，不等于内部 3D render size。应后续补 HUD 字段显示 internal render scale/size。
- 当前结论：蓝块不是 ASTC 模式、旧缓存或异步 GPU 单独导致。后续应对照 PC 正常路径和 yuzu/参考项目/Citron，重点查 TextureCache、render target aliasing、GPU->CPU/CPU->GPU dirty region、framebuffer blit/copy、barrier feedback loop/reactive flushing。
## 2026-08-16 17:19:43 +08:00 Kirby 蓝块：压缩纹理强制转码 + 参考项目 texture-cache/iterated-blend 对比
- 收尾：测试前后均执行 `adb shell am force-stop org.nxemu.app.debug`，测试后锁屏；当前手机进程已 stop，调试属性 `force_astc_transcode/force_bcn_transcode` 已复位为 false。
- 新增诊断开关：
  - `debug.nxemu.force_astc_transcode`：即使驱动报告支持 ASTC，也强制走 ASTC Converted 路径。
  - `debug.nxemu.force_bcn_transcode`：即使驱动报告支持 BCn，也强制走 BCn Converted 路径。
  - `debug.nxemu.squashed_iterated_blend`：Android 默认 true；参考 参考项目的 iterated blend workaround，可用脚本参数 `-SquashedIteratedBlend 0|1` 对比。
  - `adb-probe-nxemu.ps1` 新增 `-ForceAstcTranscode`、`-ForceBcnTranscode`、`-SquashedIteratedBlend`。
- 已移植/参考 参考项目的 texture-cache 逻辑：
  - `render_targets_serial` / `texture_bindings_serial` / feedback-loop 结果缓存；
  - active RT image 跟踪；
  - framebuffer id 缓存与 RemoveFramebuffers 失效；
  - dynamic blend enable 对 integer render target 禁用 blend；
  - fixed/dynamic iterated blend squashed workaround。
- 测试结果（realme RMX3700，T25/vulkan.purple.so，KirbyStar Allies v4.0.dnsp，NCE=true，Res=1X，FrameSkip=0，GPU accuracy=High，Reactive=true，AsyncShaders=false，清图形缓存）：
  1. `probe-script-20260816-170322`：`ForceAstcTranscode=1, ForceBcnTranscode=0`，蓝块未消失，且蓝色刷痕更明显；说明仅强制 ASTC CPU/Converted 不是正确修复。
  2. `probe-script-20260816-170437`：`ForceAstcTranscode=0, ForceBcnTranscode=1`，蓝块仍在；说明单独 BCn 转码不是根因。
  3. `probe-script-20260816-171224`：移植 texture-cache serial/feedback 后默认压缩路径，蓝块仍在。
  4. `probe-script-20260816-171746`：再启用 squashed iterated blend，蓝块仍在。
- 当前结论：
  - 蓝块依旧像透明/混合/RT 内容问题，但不是单纯 ASTC/BCn 解码开关即可解决。
  - 参考项目 texture-cache serial/feedback 与 iterated blend workaround 目前未根治 Kirby 标题页深蓝块；但这些是参考项目已有兼容逻辑，保留用于后续稳定性验证。
  - 下一步优先：对比 PC 正常路径与 Android 的 RT/clear/blit/present；增加 render target format、blend state、clear color、draw buffer、framebuffer/image id 的采样日志，确认蓝块对应的是未清 RT、alpha blend 状态、还是 shader 输出/采样边界。

## 2026-08-16 18:25:47 +08:00 Kirby 深蓝块阶段结论：T25 驱动兼容问题，26.2 正常
- 用户反馈：probe-script-20260816-181854 截图/手机画面“这次完全正常了”。该轮使用：
  - 驱动：Turnip-v26.2.0-20260418.zip，meta 显示 Turnip驱动 v306-b860e01，库 libvulkan_freedreno.so。
  - 游戏：/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp。
  - NCE=true，FrameSkip=0，Res=1X/Internal~1920x1080，GraphicsCompat=1，Reactive=true，AsyncShaders=false。
- A/B 复核：probe-script-20260816-182027 使用同一 26.2 驱动但 ForceDrawColorClear=0，截图也正常（标题页/背景无 T25 那种深蓝块残留）。
- 结论：Kirby “水/特效后留下深蓝块、角色走过才擦掉”主要是 `turnip_mrpurple_T25-raw.adpkg.zip` 驱动兼容问题，不是 ROM/缓存/keys/firmware 问题，也不是 `force_draw_color_clear` 代码开关修好的。
- 代码更新：
  - src/android/app/src/main/java/org/nxemu/app/GpuDriverHelper.kt 自动推荐排序改为优先 26.2 / v306 / b860e01，T25 降级为次选，继续降低 26.3、参考项目-fix、Biosensor/MK-I 的默认优先级。
  - src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp 保留 debug.nxemu.force_draw_color_clear 诊断开关，但默认改回 false，避免无收益降 FPS。
  - 保留 debug.nxemu.disable_accelerated_display 诊断开关；测试证明禁用 accelerated display 后 Kirby 只剩浅黄背景，说明显示内容主要来自 GPU texture-cache，不适合作为常规 fallback。
- 当前手机状态：
  - App 私有目录当前驱动已确认：Turnip驱动 v306-b860e01 / libvulkan_freedreno.so。
  - selected zip：/data/user/0/org.nxemu.app.debug/files/gpu_drivers/Turnip-v26.2.0-20260418.zip。
  - 最终 APK 已构建：src/android/app/build/outputs/apk/debug/app-debug.apk，时间约 2026-08-16 18:23:03，大小约 15.49 MB。
  - 测试后已执行 force-stop 并锁屏。
- 后续建议：
  1. 用 26.2 作为默认推荐驱动继续测 Metal Dogs、Kirby 实际关卡和更多 .dnsp/.dxci。
  2. 若其他游戏遇到黑屏/花屏，再按游戏维度做驱动矩阵，不要全局回退到 T25。
  3. `force_draw_color_clear` 仅作为特定游戏/驱动 A/B 开关，不默认打开。



## 2026-08-16 18:35:04 +08:00 Metal Dogs 26.2 驱动性能/稳定性复测
- 用户反馈：当前加载速度可以，和之前可接受速度一致；声音仍在跑，Loading 动画仍在动。
- 测试环境：realme RMX3700，驱动 Turnip-v26.2.0-20260418.zip / v306-b860e01 / libvulkan_freedreno.so，NCE=true，FrameSkip=0，Res=1X，Stretch。
- 高性能配置测试：probe-script-20260816-183048，GraphicsCompat=0、GPU acc=0、Reactive=false、AsyncShaders=true；约 50 秒后 `vulkanPresentCount` 停在 2160，session 标记 `renderStall=detected`，最终截图黑屏但 App 未崩，HUD 仍在。
- 兼容配置测试：probe-script-20260816-183259，GraphicsCompat=1、GPU acc=1、Reactive=true、AsyncShaders=false；跑到 max-wait，`vulkanPresentCount` 持续增长到 5247，未触发 renderStall。用户确认 Loading 仍动、声音仍在跑，加载速度可接受。
- 当前结论：Metal Dogs 在 26.2 驱动下可用；为了稳定应优先使用 GraphicsCompat=1/Reactive=true/AsyncShaders=false 组合。高性能组合可能触发渲染停滞，后续可做成按游戏自动兼容配置或运行时 renderStall 后提示切换兼容模式。
- 测试后已 force-stop 并锁屏。




## 2026-08-16 18:41:23 +08:00 Metal Dogs 加载速度确认 + 兼容模式默认精度补齐
- 用户确认：当前 Metal Dogs 加载速度可以，基本回到之前可接受速度；声音仍在跑，Loading 动画仍在动。因此 Metal Dogs 当前不再按“加载卡死/速度异常”处理，后续优先看稳定性和进入正式游戏后的帧率。
- 代码补齐：Android native 的图形兼容模式现在默认把 GPU accuracy 设为 High(1)，与稳定测试组合保持一致；仍可通过 debug.nxemu.gpu_accuracy 显式覆盖做 A/B。
- 当前推荐组合保持：Turnip 26.2 / v306-b860e01，NCE=true，FrameSkip=0，GraphicsCompat=true，Reactive=true，AsyncShaders=false，GPU accuracy=High。
- 下一步：构建新版 APK 后，验证不传脚本参数直接从 UI 启动 Metal Dogs 时也能自动进入该兼容 profile。

## 2026-08-16 18:43:40 +08:00 新 APK 短测：Metal Dogs 兼容路径仍正常
- 已构建并安装新版 APK：src/android/app/build/outputs/apk/debug/app-debug.apk，大小 15.49 MB，构建成功。
- 短测目录：phone-logs/probe-script-20260816-184243。
- 启动 Metal Dogs 后 App 未退出，10 秒 adaptive 探针到达 max-wait；vulkanPresentCount 从 0 增至 654，说明画面仍持续 present，未复现高性能 profile 下的 renderStall。
- session 中 NCE 已进入：nceEnabled=true / cpuBackendActual=NCE。
- session 中兼容 profile 生效：graphicsCompat=true，runtime-init 后 reactiveFlushing=true、asyncShaders=false、gpuAccuracy=1。
- 注意：这次因为用户/上次测试保存的 AppPreferences 里 graphics_compat 已经是 true，perGameProfile 日志显示 explicitOrAlreadySet；仍需后续在 graphicsCompat=false 的偏好下复测“Metal Dogs 自动拉起 compat=true”的覆盖路径。
- 测试结束已 force-stop org.nxemu.app.debug 并锁屏。

## 2026-08-16 18:45:12 +08:00 Metal Dogs 自动兼容 profile 已验证
- 为验证自动覆盖路径，先将 AppPreferences 中 graphics_compat 临时改为 false，再不传 EXTRA_GRAPHICS_COMPAT 启动 Metal Dogs。
- 短测目录：phone-logs/probe-script-20260816-184442。
- 结果：perGameProfile=metal-dogs: graphicsCompat=true; stable loading/render path，说明游戏识别后自动拉起兼容模式成功。
- runtime-init 后稳定配置生效：graphicsCompat=true、reactiveFlushing=true、asyncShaders=false、gpuAccuracy=1；NCE 进入 cpuBackendActual=NCE。
- 8 秒 adaptive 探针内 App 未退出，vulkanPresentCount 增至 390+，未出现 renderStall；测试后已 force-stop 并锁屏。
- 因运行页会保存当前性能配置，测试结束后 preferences 中 graphics_compat 已回到 true，这是符合 Metal Dogs 当前推荐设置的。

## 2026-08-16 18:56:19 +08:00 参考风格 每游戏可编辑配置第一版
- 参考 参考项目的 GamePropertiesFragment/GamePropertiesAdapter：参考项目 是“游戏详情页 + 属性卡片列表”，卡片包含 Info、Preferences/Per-game settings、Add-ons、GPU driver、Save data、Shader cache 等入口。
- 已在 nxemu Android 外壳实现第一版类似结构：
  - 新增 PerGameSettingsActivity，从首页“游戏属性 / 独立配置”进入，也可从每个游戏卡片的“属性/配置”按钮进入。
  - 首页游戏卡片从单纯选择/长按启动，改为 参考风格 卡片：显示格式徽章、大小、类型、当前 profile 摘要，并提供“启动”“属性/配置”两个操作。
  - AppPreferences 新增 per-game profile，按游戏路径 SHA-1 生成稳定 key，保存 frameSkip、resolution、aspect、graphicsCompat、preferNce、perfHudDetailed、autoOutputLog、driverSource。
  - MainActivity.startEmulation() 会读取本游戏 profile；如果 enabled=true，就通过 Intent 覆盖全局性能设置。
  - EmulationActivity 新增 EXTRA_GAME_NAME 和 EXTRA_GPU_DRIVER_SOURCE；启动时可先应用本游戏绑定驱动，再初始化 GPU/runtime。
  - GpuDriverHelper 新增 currentSelectedDriverSource()/useDriverSource()，支持把当前驱动绑定到某个游戏，或回到全局。
- UI 使用方式：
  1. 首页扫描/选择游戏。
  2. 点游戏卡片只选择；点卡片里的“启动”直接跑；点“属性/配置”进入该游戏设置页。
  3. 在设置页切换任意项会自动启用“独立配置”；点“保存并启动”会用本游戏配置启动。
  4. “本游戏驱动”会把当前已安装驱动绑定到该游戏；“清除本游戏驱动绑定”改回跟随全局。
- 已构建成功并安装到 realme RMX3700；直接 adb 启动 PerGameSettingsActivity 无 FATAL/AndroidRuntime 崩溃。测试后已 force-stop 并锁屏。
- APK：src/android/app/build/outputs/apk/debug/app-debug.apk，时间约 2026-08-16 18:55:39，大小约 15.50 MB。
- 后续仍需继续向 参考项目 靠近：Material/RecyclerView/Navigation 化、真正封面/图标、游戏信息页、存档/清 shader、驱动管理页 per-game 选择列表，而不是当前的程序化简化卡片。

## 2026-08-16 19:12:22 +08:00 Per-game 属性页继续 参考项目 化：驱动列表/维护卡片
- 本轮继续向 参考项目 GameProperties 页靠近，补了更像“属性卡片列表”的功能：
  - Game info 卡：显示游戏名、路径、格式、大小和配置 ID。
  - GPU driver 卡：不再只能绑定当前驱动，新增 GpuDriverHelper.listDriverChoices()，可列出“跟随全局/系统默认”、已安装 zip、以及 /sdcard/ns/qudong 默认目录候选。
  - Per-game driver 现在通过弹窗选择，选择后写入该游戏 profile 的 driverSource。
  - 保留“绑定当前全局驱动”快捷入口。
  - Maintenance 区新增“清理图形/Shader 缓存”和“复制配置摘要”。
- GpuDriverHelper 新增：
  - DriverChoice(label, source, detail)；
  - listDriverChoices()，用于 UI 展示 per-game 可选驱动；
  - 继续沿用 useDriverSource() 在启动游戏前切换本游戏绑定驱动。
- 构建：app-debug.apk 构建成功，约 15.5 MB。
- 真机验证：已安装到 realme RMX3700，adb 直接启动 PerGameSettingsActivity 可进入，App 有 pid，无 nxemu 自身 FATAL/AndroidRuntime 崩溃；系统日志里有 realme/Google Play 噪声但非本 app 崩溃。测试后已 force-stop 并锁屏。
- 当前 UI 与 参考项目的差距：仍是程序化 View + AlertDialog；下一步应引入/复用 RecyclerView/Material 风格，做首页封面网格、游戏信息页、驱动管理页、存档/Addon/Shader 单独页面。


## 2026-08-16 19:17:18 +08:00 Per-game 推荐配置/TitleID 识别
- 继续补 参考风格 游戏属性页：
  - 首页游戏卡片和 per-game 属性页现在会从文件名/路径中提取 TitleID（如 0100A6E01681C000）并显示。
  - PerGameSettingsActivity 新增 Recommended profile 区：按游戏名/TitleID 匹配推荐配置，一键写入该游戏独立 profile。
  - 当前内置推荐：
    - Metal Dogs / 0100A6E01681C000：NCE=true、1X、拉伸全屏、FrameSkip=0、GraphicsCompat=true、HUD 精简、自动日志关，并优先绑定可找到的 Turnip 26.2/v306/b860e01 驱动。
    - Kirby/星之卡比：NCE=true、1X、拉伸全屏、FrameSkip=0、GraphicsCompat=true，并优先绑定 Turnip 26.2/v306/b860e01，避开 T25/26.3 的蓝块/黑屏问题。
  - Game info 卡现在显示格式、大小、TitleID。
- 构建成功：app-debug.apk，构建日志 src/android/build-last.log，无 Kotlin/Java 编译错误。
- 真机短测：已安装到 realme RMX3700，直接启动 Metal Dogs 的 PerGameSettingsActivity，页面 HAS_DRAWN，有 app pid，无 FATAL EXCEPTION/AndroidRuntime 崩溃；测试后已 force-stop 并锁屏。
- 下一步：把推荐配置从硬编码 Kotlin 迁移到可扩展 JSON/数据库；继续做 参考项目 式封面网格、游戏详情分页、驱动管理列表页，而不是弹窗。


## 2026-08-16 19:23:41 +08:00 推荐配置迁移到可编辑 JSON
- 新增 GameProfileCatalog.kt，把 per-game 推荐配置从 PerGameSettingsActivity 硬编码迁移为可编辑 JSON：
  - 优先路径：/sdcard/ns/config/nxemu_game_profiles.json。
  - 如果外置路径不可写，则退回 app 私有 files/config/nxemu_game_profiles.json。
  - 首次进入游戏属性页会自动生成默认 JSON。
- JSON 结构：profiles[]，每条支持：
  - id、enabled、title、description；
  - titleIds、nameContains 匹配规则；
  - settings.frameSkip/resolution/aspect/graphicsCompat/preferNce/perfHudDetailed/autoOutputLog/driverHint。
- 当前默认 JSON 内置：
  - metal-dogs-stable，匹配 0100A6E01681C000 / metal dogs；
  - kirby-star-allies-26-2，匹配 kirby / 星之卡比。
- driverHint 支持 turnip26.2、26.2、v306、b860e01、t25、purple 或实际文件路径；会通过 GpuDriverHelper.listDriverChoices() 解析为实际 source。
- PerGameSettingsActivity 现在从 JSON 读取 Recommended profile，并显示配置库路径；Maintenance 区新增“推荐配置 JSON / 复制路径”。
- 构建成功；真机安装后打开 Metal Dogs 属性页，页面正常 HAS_DRAWN，无 nxemu FATAL/AndroidRuntime 崩溃。已确认手机上生成：/sdcard/ns/config/nxemu_game_profiles.json，大小约 1308 bytes。测试后已 force-stop 并锁屏。
- 下一步：做真正的 JSON 编辑/导入导出 UI，或者先继续 参考风格 首页封面网格和游戏详情分页。



## 2026-08-16 19:28:36 +08:00 推荐配置 JSON 编辑器/重置入口
- PerGameSettingsActivity 的 Maintenance 区继续增强：
  - “推荐配置 JSON”现在不是只复制路径，而是直接打开内置 JSON 编辑器。
  - 编辑器使用多行等宽文本框，支持保存前校验：必须是 JSON 对象、必须有 profiles 数组、每条 profile 必须有 id 和 settings，frameSkip 必须 0..4。
  - 保存时会用 JSONObject(text).toString(2) 格式化后写回 /sdcard/ns/config/nxemu_game_profiles.json，并刷新当前推荐 profile。
  - 新增“复制推荐配置路径”。
  - 新增“重置推荐配置模板”，可恢复内置 Metal Dogs/Kirby 默认 JSON，会覆盖当前配置库。
- GameProfileCatalog.defaultJson() 改为 public，供 UI 重置模板使用。
- 构建成功：src/android/app/build/outputs/apk/debug/app-debug.apk。
- 真机短测：安装后打开 Metal Dogs 属性页，页面 HAS_DRAWN，存在 app pid，无 FATAL EXCEPTION/AndroidRuntime 崩溃；测试后已 force-stop 并锁屏。
- 后续：如果要继续 参考项目 化，下一步建议做首页封面网格/游戏详情分页；如果要继续实用功能，下一步做 JSON 导入/导出和 profile 列表管理。


## 2026-08-16 19:31:37 +08:00 推荐配置列表管理/剪贴板导入导出
- GameProfileCatalog 新增：
  - listPresets(context)：读取 JSON 中全部 profiles，返回可套用的 Preset 列表。
  - validateJson(text)：统一校验 JSON 对象、profiles 数组、profile id/settings、frameSkip 范围。
- PerGameSettingsActivity 的 Maintenance 区新增：
  - “推荐配置列表”：展示 JSON 中全部 profile，点击任意项可强制套用到当前游戏，不要求匹配 TitleID/nameContains。
  - “复制推荐配置内容”：复制整份 JSON 到剪贴板，方便备份/发给我。
  - “从剪贴板导入 JSON”：读取剪贴板，校验通过后覆盖 /sdcard/ns/config/nxemu_game_profiles.json，并刷新当前推荐配置。
- 原 JSON 编辑器改用 GameProfileCatalog.validateJson()，避免校验逻辑散落在 Activity。
- 构建成功：app-debug.apk。
- 真机短测：已安装到 realme RMX3700，打开 Metal Dogs 属性页 HAS_DRAWN，有 app pid，无 FATAL EXCEPTION/AndroidRuntime 崩溃；测试后已 force-stop 并锁屏。
- 后续建议：继续 参考风格 首页封面网格/分页详情；或者补 JSON profile 的新增/删除单项 UI，避免每次编辑整份 JSON。


## 2026-08-16 19:37:04 +08:00 JSON profile 单项保存/删除 UI
- GameProfileCatalog 新增：
  - savePresetForGame(context, gameName, gamePath, profile)：把当前游戏 profile 写入 /sdcard/ns/config/nxemu_game_profiles.json。id 优先用 TitleID，例如 0100a6e01681c000-custom；无 TitleID 时用文件名 slug。若 id 已存在则替换，否则追加。
  - deletePreset(context, id)：按 id 从 JSON 的 profiles[] 中删除单条 profile。
  - profileToJson()：把当前 AppPreferences.GameProfile 序列化为 JSON settings，保留 resolution/aspect 的数字和可读标签。
- PerGameSettingsActivity 的 Maintenance 区新增：
  - “保存当前为推荐配置”：把当前本游戏独立配置保存成可自动匹配的 JSON profile。
  - “删除推荐配置”：列出 JSON 中全部 profile，二次确认后删除选中项。
- 这使常见流程变成：调好某游戏 per-game 设置 -> 保存当前为推荐配置 -> 下次该 TitleID/文件名自动出现推荐卡片。
- 构建成功：app-debug.apk。
- 真机短测：已安装到 realme RMX3700，打开 Metal Dogs 属性页 HAS_DRAWN，有 app pid，无 FATAL EXCEPTION/AndroidRuntime 崩溃；测试后已 force-stop 并锁屏。
- 后续：可以继续做 参考风格 首页封面网格/分页详情，或者把 JSON profile 单项新增做成表单而不是从当前配置保存。


## 2026-08-16 20:50:45 +08:00 参考项目 UI 对照复核 + 当前进度确认
- 用户反馈：当前 nxemu Android 外壳与 参考项目 UI 差距仍很远；本轮重新对照本地 参考项目 实现确认。
- 参考项目 参考文件已复查：
  - D:\project\switch-emulator-refs\参考项目\src\android\app\src\main\res\layout\activity_main.xml
  - ...\res\layout\fragment_games.xml
  - ...\res\layout\card_game_grid.xml
  - ...\res\layout\fragment_game_properties.xml
  - ...\java\org\yuzu\yuzu_emu\ui\GamesFragment.kt
  - ...\java\org\yuzu\yuzu_emu\adapters\GameAdapter.kt
  - ...\java\org\yuzu\yuzu_emu\fragments\GamePropertiesFragment.kt
  - ...\java\org\yuzu\yuzu_emu\adapters\GamePropertiesAdapter.kt
- 参考项目 UI/架构要点：
  - `activity_main.xml` 是 ConstraintLayout + FragmentContainerView + NavHostFragment + home_navigation。
  - 首页 `fragment_games.xml` 有 参考项目 header、圆形 view/filter/settings 按钮、搜索框、SwipeRefreshLayout + RecyclerView、FAB/add-directory。
  - 游戏卡片 card_game_grid.xml 是 GradientBorderCardView + ShapeableImageView，150dp 封面图 + 单行标题，Grid/List/Compact/Carousel 由 GameAdapter 切换。
  - 游戏属性页 `fragment_game_properties.xml` 是封面大图、标题、playtime、属性 RecyclerView、Extended FAB Start。
  - GamePropertiesAdapter 使用 SubmenuProperty / InstallableProperty 两类卡片，支持二级操作按钮，Addon/Save/Shader/Driver/Shortcut 等入口可以统一挂载。
  - 参考项目 Gradle 依赖/能力包括：ViewBinding=true、AppCompat、RecyclerView、ConstraintLayout、Fragment KTX、Material、SwipeRefreshLayout、Navigation Fragment/UI、SafeArgs。
- nxemu 当前状态复核：
  - src/android/app/build.gradle.kts 目前 `viewBinding=false`，未引入 AndroidX Navigation/Fragment/RecyclerView/Material/SwipeRefreshLayout/ConstraintLayout 这一套。
  - MainActivity.kt 仍是单 Activity 程序化 ScrollView + LinearLayout + Button/TextView 外壳；已有 参考风格 文案和卡片，但不是 参考项目的 Fragment/RecyclerView/XML 架构。
  - PerGameSettingsActivity.kt 也是程序化 ScrollView + propertyCard，功能接近 参考项目 属性卡片，但视觉、动效、封面、分组、Adapter、二级操作仍差很远。
  - EmulationActivity.kt 运行页已有 Surface、HUD、触控按键、性能配置按钮、日志保存、NCE/分辨率/跳帧/图形兼容切换，但也仍是程序化 View。
- 当前进度重新估计：
  - APK 构建/安装/启动：90%。
  - .dnsp/.dxci 路径接入与 last-game/per-game 启动：65%。
  - Metal Dogs 可玩链路：65~75%，仍慢但可运行。
  - Kirby/星之卡比显示兼容：70%，26.2 驱动基本正常；T25/26.3/MK-I 仍有兼容差异。
  - NCE：60~70%，session/HUD 可显示 cpuBackendActual=NCE，但性能仍需继续 profiling 和补热点。
  - GPU 驱动管理：55~60%，可安装/选择/记忆/绑定 per-game；还不是 参考项目的 DriverManager 页面。
  - Per-game profile：65%，已支持本游戏配置、推荐 JSON、导入导出、保存/删除 preset；还缺 参考项目 式页面/Adapter 和更多配置项。
  - 日志/诊断：75%，已有自动日志、复制/保存、session/perf/renderStall 信息。
  - 触控/手柄：55%，触控布局已补全许多键，仍需更像 参考项目的 overlay editor/profile；外接手柄链路还未系统化测试。
  - 参考风格 UI 外壳：25~30%，功能壳有了，但架构/视觉远未达到 参考项目。
  - 游戏库/封面/详情：25%，可扫描和显示卡片，无真实封面/图标、无 Grid/List/Carousel、无 playtime。
  - 存档/Addon/Shader 管理：10~15%，只有清缓存/维护入口，未做完整管理页。
  - 总体离“像参考项目 且能较稳定玩游戏”：约 35~45%。
- 建议后续 UI 路线：
  1. 先不再继续堆程序化 UI；转为引入 AndroidX/Material/ViewBinding/XML。
  2. 第一阶段只重构外壳首页，不碰 core：新增 `activity_main.xml`、`fragment_games.xml`、card_game_grid.xml、GameEntryAdapter，保留现有 MainActivity.startEmulation() 与扫描逻辑。
  3. 第二阶段重构游戏详情页：新增 `fragment_game_properties.xml` 风格页面/或新 Activity XML，复用现有 PerGameSettingsActivity 的 profile/driver/catalog 逻辑。
  4. 第三阶段拆 DriverManager、Settings、Shader/Save/Add-on 页面。
  5. 每阶段都保持 APK 可构建、可安装、可从 /sdcard/ns/rom(s) 启动游戏，避免 UI 重构破坏当前可玩链路。

## 2026-08-16 21:21:49 +08:00 正式转向 参考项目 架构：首页 XML/ViewBinding/RecyclerView 第一阶段
- 用户确认：正式转向 参考项目 架构，不再继续只堆程序化 UI。
- 本轮完成第一阶段首页重构，目标是先把外壳迁移到 参考风格 Android UI 基础，但不破坏现有 core/启动链路。
- Gradle/UI 基础：
  - src/android/app/build.gradle.kts：`viewBinding=false` 改为 `viewBinding=true`。
  - 新增依赖：`androidx.recyclerview:recyclerview:1.4.0`。
- 新增 参考风格 资源：
  - `res/layout/activity_main.xml`：首页从纯程序化 View 改为 XML + ViewBinding；包含 header、搜索、扫描/授权/更多按钮、RecyclerView 游戏库、诊断面板。
  - `res/layout/item_game_card.xml`：游戏卡片 item，包含格式 badge、标题、详情、profile 摘要、启动/属性按钮。
  - `res/drawable/参考项目_background_gradient.xml`、`参考项目_panel.xml`、`参考项目_hero.xml`、`参考项目_search_bg.xml`、`参考项目_game_card.xml`、`参考项目_game_card_selected.xml`、`参考项目_button*.xml`、`参考项目_badge.xml`。
  - `res/values/styles.xml` 新增 NxEmu参考项目Button / NxEmu参考项目ButtonPrimary。
- MainActivity 改动：
  - 新增 ActivityMainBinding 和 ItemGameCardBinding。
  - 新增 setup参考项目Home()，首页使用 XML binding 初始化。
  - 新增 参考项目GameAdapter : RecyclerView.Adapter，替代首页手工 LinearLayout chunked 卡片渲染。
  - 首页游戏库现在使用 GridLayoutManager(this, 3)。
  - 搜索过滤、选择游戏、长按启动、启动按钮、属性/配置按钮保留。
  - “更多”按钮临时用 PopupMenu 承载原先的选择文件、运行上次、驱动安装/选择、清缓存、自动日志、诊断开关、复制日志、授权所有文件等入口。
  - 保留 startEmulation()、openPerGameSettings()、per-game profile、驱动绑定、日志等原链路。
- 构建验证：
  - :app:compileDebugKotlin 已通过。
  - :app:assembleDebug 已通过。
  - APK：D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk
  - APK 大小约 17.30 MB，时间 2026-08-16 21:20:16。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 org.nxemu.app.debug/org.nxemu.app.MainActivity 后进程存在，无 AndroidRuntime/FATAL EXCEPTION 命中。
  - 本轮只做首页短测，没有启动游戏。
  - 测试后已执行 `adb shell am force-stop org.nxemu.app.debug` 并锁屏，pidof 无输出。
- 当前 UI 进度变化：
  - 参考风格 UI 外壳从约 25~30% 提升到约 35%。
  - 已进入 XML/ViewBinding/RecyclerView 路线，但还不是完整 参考项目 Fragment/Navigation/Material 架构。
- 下一步建议：
  1. 首页继续 参考项目 化：加入真实 Material/ConstraintLayout、封面占位/游戏图标、Grid/List/Compact 切换、横竖屏列数适配。
  2. 把 “更多” PopupMenu 拆成 参考项目 式设置/驱动/诊断页面。
  3. 把 PerGameSettingsActivity 迁移到 XML + RecyclerView 属性页，复刻 参考项目 `fragment_game_properties.xml` + GamePropertiesAdapter 思路。
  4. 后续再引入 Fragment/Navigation，不建议一口气全量改，避免影响当前可玩链路。

## 2026-08-16 21:27:47 +08:00 参考项目 架构第二步：游戏属性页 XML/ViewBinding/RecyclerView
- 本轮继续正式转向 参考项目 架构，把 `PerGameSettingsActivity` 从程序化 `ScrollView + LinearLayout + propertyCard` 迁移到 XML + ViewBinding + RecyclerView 属性列表。
- 新增资源：
  - `res/layout/activity_per_game_settings.xml`：参考风格 游戏属性页骨架，包含大 header、游戏格式/图标 badge、标题、路径、summary、保存并启动/返回/重置按钮、属性 RecyclerView。
  - `res/layout/item_property_card.xml`：属性卡片 item，包含 title/description/detail，点击执行对应 action。
  - `res/layout/item_property_header.xml`：属性分组 header item。
- Kotlin 重构：
  - `PerGameSettingsActivity` 新增 `ActivityPerGameSettingsBinding`。
  - 新增 `PropertyItem.Header` / `PropertyItem.Card`。
  - 新增 `PropertyAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>`。
  - `refreshUi()` 现在构建 property item 列表并 `propertyAdapter.submit(items)`，不再向 LinearLayout 动态 addView。
  - 保留全部原有业务逻辑：推荐 profile、独立配置、分辨率、画面比例、跳帧、图形兼容、NCE、HUD、自动日志、per-game GPU driver、推荐配置 JSON、清理图形缓存、复制配置摘要、保存并启动。
- 构建验证：`:app:compileDebugKotlin` 和 `:app:assembleDebug` 均通过。
- APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`，大小约 17.30 MB，时间 2026-08-16 21:26:23。
- 真机短测：已安装到 realme RMX3700 / b72182d；adb 启动 `org.nxemu.app.debug/org.nxemu.app.PerGameSettingsActivity` 并传入 Metal Dogs 路径后，app pid 存在，无 `AndroidRuntime/FATAL EXCEPTION`；本轮只测属性页打开，没有启动游戏；测试后已 force-stop 并锁屏。
- 当前 UI 进度变化：参考风格 UI 外壳/属性页从约 35% 提升到约 40%；首页和属性页都已进入 XML/ViewBinding/RecyclerView 路线。
- 下一步建议：
  1. 首页补 Grid/List/Compact 切换和封面/图标占位，继续靠近 参考项目 `GameAdapter`。
  2. 属性页进一步接近 参考项目：加大封面/游戏图标、Start FAB、二级 action buttons、playtime。
  3. 把 GPU Driver 管理从弹窗拆成独立 DriverManager 页面。
  4. 保持每次改 UI 后只做短测，避免破坏当前 DNSP/DXCI 启动链路。

## 2026-08-16 21:34:15 +08:00 参考项目首页 GameAdapter 第三步：Grid/List/Compact 视图切换
- 本轮继续向 参考项目 GameAdapter 靠近，给首页游戏库增加视图模式切换。
- 新增/修改：
  - `AppPreferences` 新增 `homeViewMode()` / `saveHomeViewMode()`，持久化首页视图模式。
  - `activity_main.xml` 新增 `button_view_mode`，放在搜索栏右侧，显示当前 Grid / List / Compact。
  - `item_game_card.xml` 给内容行和操作行增加 id：`content_row`、`actions_row`，便于按模式调整显示。
  - `MainActivity` 新增 `homeViewMode` 状态，启动时恢复上次模式。
  - 新增 `applyHomeViewMode()`：
    - Grid：3 列，标准卡片。
    - List：1 列，更像列表视图。
    - Compact：4 列，隐藏 profile 和操作按钮，卡片更小。
  - `参考项目GameAdapter` 新增 `setViewMode()` 和 `applyModeLayout()`，根据模式调整 badge 尺寸、字体、profile/action 可见性。
  - “更多”菜单中也加入“切换视图：Grid/List/Compact”。
- 构建验证：
  - :app:compileDebugKotlin 通过。
  - :app:assembleDebug 通过。
  - APK：D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk
  - APK 大小约 17.31 MB，时间 2026-08-16 21:32:54。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 org.nxemu.app.debug/org.nxemu.app.MainActivity 后 app pid 存在，无 AndroidRuntime/FATAL EXCEPTION。
  - 本轮只测首页打开，没有启动游戏。
  - 测试后已 force-stop 并锁屏。
- 当前 UI 进度变化：
  - 参考风格 UI 从约 40% 提升到约 42%。
  - 已有 参考项目 类似的首页多视图模式雏形，但还缺真实封面/图标、DiffAdapter、排序/过滤 popup、Carousel、Material 组件。
- 下一步建议：
  1. 做封面/图标：先用 ext badge 占位，再尝试从 native/probe 或缓存目录读取 title icon。
  2. 做排序/过滤菜单：按名称、大小、格式、TitleID、本体/更新/DLC 概率排序。
  3. 把 GPU Driver 管理拆成独立页面，继续减少“更多”弹窗负担。
  4. 属性页继续加 Start FAB、二级 action buttons 和 playtime。


## 2026-08-16 21:39:56 +08:00 参考项目首页第四步：排序/过滤菜单
- 本轮继续向 参考项目首页靠近，新增排序/过滤能力。
- 新增/修改：
  - `AppPreferences` 新增 `homeSortMode()` / `saveHomeSortMode()`，保存首页排序方式。
  - `activity_main.xml` 新增 `button_sort_mode`，位于搜索栏右侧，显示当前排序标签。
  - `MainActivity` 新增 `homeSortMode` 状态，启动时恢复上次排序。
  - 新增 `showSortModeMenu()`：弹出排序菜单。
  - 新增 `sortModeLabel()` / `sortGames()` / `formatSortScore()`。
  - 搜索过滤现在也匹配 TitleID。
  - 
otice_text 会显示当前排序方式。
  - “更多”菜单中也加入当前排序入口。
- 当前排序模式：
  - 推荐 / 本体优先：DNSP/DXCI + v0 本体优先，更新/DLC 倾向后排。
  - 名称 A-Z。
  - 名称 Z-A。
  - 大小 大-小。
  - 大小 小-大。
  - 格式 DNSP/DXCI 优先。
- 构建验证：
  - :app:compileDebugKotlin 通过。
  - :app:assembleDebug 通过。
  - APK：D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk
  - APK 大小约 17.31 MB，时间 2026-08-16 21:38:14。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 org.nxemu.app.debug/org.nxemu.app.MainActivity 后 app pid 存在，无 AndroidRuntime/FATAL EXCEPTION。
  - 本轮只测首页打开，没有启动游戏。
  - 测试后已 force-stop 并锁屏。
- 当前 UI 进度变化：
  - 参考风格 UI 从约 42% 提升到约 44%。
  - 首页已具备搜索、多视图、排序基础，继续靠近 参考项目的 GamesFragment/GameAdapter。
- 下一步建议：
  1. 做游戏封面/图标缓存和显示。
  2. 将 `PopupMenu` 继续拆出独立 Settings/Driver 页面。
  3. 首页列表改用 DiffUtil/ListAdapter，减少刷新闪烁。
  4. 属性页继续做 Start FAB、二级 action buttons、playtime。

## 2026-08-16 21:48:12 +08:00 参考项目 外壳第五步：独立 GPU Driver Manager 页面
- 本轮继续“正式转向 参考项目 架构”，优先把驱动管理从首页 `PopupMenu` 中拆成独立页面，参考 参考项目 `DriverManagerFragment` / `DriverAdapter` 的思路：独立页面、卡片列表、当前驱动单选态、安装/自动推荐/更多操作入口。
- 新增/修改：
  - `src/android/app/src/main/java/org/nxemu/app/DriverManagerActivity.kt`
  - `src/android/app/src/main/res/layout/activity_driver_manager.xml`
  - `src/android/app/src/main/res/layout/item_driver_card.xml`
  - `AndroidManifest.xml` 注册 `.DriverManagerActivity`
  - `activity_main.xml` 首页顶部新增“驱动”按钮。
  - `MainActivity` 新增 `openDriverManager()`，首页按钮和“更多”菜单都可进入独立驱动页。
- 新驱动页功能：
  - 显示当前驱动：系统默认或已选择 ZIP/目录。
  - 列出 `GpuDriverHelper.listDriverChoices()` 中的系统默认、已存储驱动、`/sdcard/ns/qudong` 候选驱动。
  - 点击卡片切换驱动，沿用 `GpuDriverHelper.useDriverSource()`，会记录最后一次选择。
  - 支持“安装ZIP”“自动推荐”“授权驱动目录”“恢复系统 GPU 驱动”“清理图形/驱动缓存”“授权所有文件访问”“复制驱动诊断”。
  - 仍保留推荐结论：RMX3700/Adreno 7xx 当前优先 Turnip 26.2/v306-b860e01；26.3/T25/MK-I 仅作兼容对比。
- 构建验证：
  - `:app:compileDebugKotlin` 通过。
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.52 MB，时间：2026-08-16 21:46:58。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 `org.nxemu.app.debug/org.nxemu.app.MainActivity`：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 启动 `org.nxemu.app.debug/org.nxemu.app.DriverManagerActivity`：页面创建成功，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 本轮没有启动游戏，只验证外壳 UI 和驱动页不崩。
  - 测试后已执行 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 44% 提升到约 47%。
  - 驱动管理从约 55~60% 提升到约 62%，主要提升在 UI/可操作性和最后选择恢复链路展示。
- 下一步建议：
  1. 继续 参考项目 化首页：游戏封面/图标缓存、DiffUtil/ListAdapter、空状态/最近游戏/封面墙。
  2. 继续 参考项目 化属性页：Start FAB、二级功能入口、存档/Add-on/Shader 管理入口。
  3. 再回到 NCE 性能慢与 HLE/NVDRV/VI 日志降噪；不要在 UI 重构期间破坏可玩链路。

## 2026-08-16 21:50:59 +08:00 参考项目首页第六步：GameAdapter DiffUtil 刷新
- 本轮继续向 参考项目 `GameAdapter` 靠近，首页游戏列表从全量 `notifyDataSetChanged()` 改为 `DiffUtil.calculateDiff()` 后分发更新。
- 修改：
  - `MainActivity.kt` 引入 `androidx.recyclerview.widget.DiffUtil`。
  - `参考项目GameAdapter.submit()` 改为比较旧/新列表：以 `uri` 判定同一游戏，以 `GameEntry` 内容 + 选中态判定内容是否相同。
- 目的：
  - 搜索、排序、Grid/List/Compact 切换、选中游戏时减少列表整体闪烁。
  - 为后续封面缓存、真实图标、最近游戏/分类列表做基础。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.52 MB，时间：2026-08-16 21:50:05。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 `org.nxemu.app.debug/org.nxemu.app.MainActivity`：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 本轮仅验证首页，不启动游戏。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 47% 提升到约 48%。
- 下一步建议：
  1. 首页补真实封面/图标缓存或更好的占位封面。
  2. 属性页继续 Start FAB/二级入口。
  3. 完成 UI 外壳后再集中回到 NCE 性能慢、HLE/NVDRV/VI 日志降噪和游戏兼容。

## 2026-08-16 22:02:20 +08:00 参考项目首页第七步：游戏封面/图标缓存基础
- 本轮继续首页 参考项目 化，参考 参考项目 `card_game_grid.xml` 的封面卡片思路，在当前 `item_game_card.xml` 中加入封面层。
- 新增/修改：
  - `item_game_card.xml`：原 `image_badge` 外包 `cover_frame`，新增 `ImageView game_cover`；有封面时显示图片，无封面时继续显示 DNSP/DXCI/NRO badge。
  - `MainActivity.kt`：新增 `findGameCover(entry)` 与 `sanitizeCoverKey(text)`。
  - `参考项目GameAdapter.bind()`：绑定时尝试查找封面并 `setImageURI(Uri.fromFile(cover))`。
  - Grid/List/Compact 模式现在调整 `cover_frame` 尺寸，不再只调 `image_badge`。
- 当前封面查找规则：
  - 优先按 TitleID，例如 `0100XXXXXXXXXXXX.png/jpg/jpeg/webp`。
  - 其次按游戏文件名规范化，例如 `Metal Dogs [0100...][v0][JP].dnsp` 会尝试规范化后的文件名。
  - 支持目录：
    - `/sdcard/ns/covers`
    - `/sdcard/ns/cover`
    - `/sdcard/ns/rom/covers`
    - `/sdcard/ns/roms/covers`
    - app 私有 `files/covers`
  - 当前是本地封面缓存/占位基础，还没有联网抓取，也没有从 NSP/NCA icon 提取真实图标。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.52 MB，时间：2026-08-16 22:01:18。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 `org.nxemu.app.debug/org.nxemu.app.MainActivity`：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 本轮只测首页/资源/ViewBinding，不启动游戏。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 48% 提升到约 50%。
  - 首页已有搜索、排序、多视图、DiffUtil、独立驱动页入口、封面缓存基础。
- 下一步建议：
  1. 做真正的游戏详情/信息页或属性页 Start FAB/二级入口。
  2. 增加封面导入/刷新入口，或从 title icon/cache/native probe 中提取真实图标。
  3. UI 外壳再推进一轮后，回到 NCE 性能慢与 HLE/NVDRV/VI 日志降噪。

## 2026-08-16 22:09:28 +08:00 参考项目 属性页第三步：底部启动 FAB/快捷工具栏
- 本轮继续属性页 参考项目 化，参考 参考项目 `fragment_game_properties.xml` 的底部 `ExtendedFloatingActionButton` 思路，把“保存并启动”从右侧普通按钮改成底部固定启动入口。
- 新增/修改：
  - `activity_per_game_settings.xml` 根布局改为 `FrameLayout`。
  - 内容区增加底部 padding，避免列表被底部栏遮挡。
  - 新增底部栏 `bottom_bar`：
    - `button_quick_driver`：快速打开本游戏 GPU driver 选择。
    - `button_quick_log`：快速开关本游戏自动日志。
    - `button_quick_shader`：快速清理图形/Shader 缓存。
    - `bottom_hint`：显示当前独立配置/NCE/分辨率/驱动摘要。
    - `button_start`：底部右侧 “▶ 启动”，更接近 参考项目 FAB 的主操作。
  - `PerGameSettingsActivity.kt` 新增：
    - `toggleAutoLogQuick()`
    - `clearGraphicsCacheQuick()`
    - refresh 时同步底部摘要和日志按钮文字。
- 保留链路：
  - `launchGame()` 未改启动参数语义，仍传入 per-game frameSkip/resolution/aspect/graphicsCompat/NCE/HUD/autoLog/driverSource。
  - 重置、驱动选择、推荐配置、JSON 编辑、清缓存、复制摘要等原功能保留。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.52 MB，时间：2026-08-16 22:08:29。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 `org.nxemu.app.debug/org.nxemu.app.PerGameSettingsActivity` 并传入 Metal Dogs 路径：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION` / `InflateException`。
  - 本轮只测属性页打开，不启动游戏。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 50% 提升到约 52%。
  - 属性页已具备 参考风格 header、RecyclerView property list、底部启动主入口、快捷驱动/日志/缓存入口。
- 下一步建议：
  1. 属性页再补封面图真实显示、playtime 占位、Shortcut/Info/Save/Add-on/Shader 子页面入口。
  2. 首页增加封面导入/刷新入口。
  3. UI 外壳完成到约 55% 后回到 NCE 性能慢和日志降噪。

## 2026-08-16 22:21:37 +08:00 UI 100% 路线第八步：属性页封面/playtime + GameManagement 二级页
- 用户要求“继续，直到 UI 100%”。本轮继续补 参考项目/yuzu 属性页的关键结构：封面、playtime、二级管理页面。
- 新增/修改：
  - `activity_per_game_settings.xml`：左侧 `game_icon` 改为封面 FrameLayout，新增 `game_cover` 和 `playtime`。
  - `PerGameSettingsActivity.kt`：新增 `applyCoverAndPlaytime()` / `findGameCover()` / `sanitizeCoverKey()` / `playtimeText()`。
  - `PerGameSettingsActivity.kt`：新增 `openManagement(mode)`，并在属性列表中加入二级入口：
    - 游戏信息
    - 存档管理
    - Add-on / DLC
    - Shader 缓存
    - 日志
  - 新增 `GameManagementActivity.kt`：通用 参考风格 二级管理页壳。
  - 新增 `activity_game_management.xml`：二级页 header + RecyclerView + 复制诊断/返回按钮。
  - `AndroidManifest.xml` 注册 `.GameManagementActivity`。
- 二级页当前能力：
  - `info`：显示名称、路径、大小、TitleID、profile 摘要。
  - `save`：展示 TitleID、私有 save 根路径，预留导入/导出入口。
  - `addon`：展示 Add-on/DLC 管理壳，扫描 `/sdcard/ns/addons` 数量，预留安装/启用入口。
  - `shader`：列出 shader/pipeline/vulkan cache 目录大小，并可调用清理缓存。
  - `log`：展示 `/sdcard/ns/logs` 最近日志，可复制日志尾部和运行诊断。
- 封面/playtime：
  - 封面规则与首页一致，优先 `/sdcard/ns/covers` 等目录中的 TitleID 或规范化文件名图片。
  - playtime 先读 app 私有 `files/playtime/<perGameKey>.txt` 秒数；当前是 UI 占位，后续运行页再写入累计时长。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.54 MB，时间：2026-08-16 22:20:20。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 打开 `PerGameSettingsActivity` + `GameManagementActivity(shader)` + `GameManagementActivity(log)`：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION` / `InflateException`。
  - 本轮没有启动游戏。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 52% 提升到约 58%。
  - 已有：首页搜索/排序/多视图/DiffUtil/封面基础；属性页 header/封面/playtime/底部启动/快捷入口/二级管理页；独立驱动页。
- UI 100% 剩余大项：
  1. 首页真正封面墙/最近游戏/空状态/导入封面入口。
  2. 设置主页面/全局设置页面（图形、系统、输入、音频、调试）。
  3. 输入映射/手柄页面。
  4. Add-on/Save/Shader 页面从 shell 变成可实际安装/导入/导出。
  5. 运行页 overlay 美化：菜单、暂停、退出确认、控制布局编辑。
  6. 统一导航/返回栈/过渡动画，逐步接近 参考项目 Fragment/Navigation 体验。

## 2026-08-16 22:24:53 +08:00 UI 100% 路线第九步：参考风格 全局设置页
- 本轮补 UI 100% 大项之一：全局 Settings 页面，用于承载图形/性能、驱动、日志、存储、输入等全局入口。
- 新增/修改：
  - 新增 `SettingsActivity.kt`。
  - 新增 `activity_settings.xml`。
  - `AndroidManifest.xml` 注册 `.SettingsActivity`。
  - `activity_main.xml` 首页顶部新增“设置”按钮。
  - `MainActivity.kt` 新增 `openSettings()`，首页按钮和“更多 -> 全局设置”均可进入。
- 设置页当前分组：
  - Performance / Graphics：全局分辨率、画面比例、跳帧、图形兼容、NCE、性能 HUD。
  - Drivers / Logs：GPU Driver Manager、清理图形/Shader 缓存、自动日志。
  - Library / Storage：上次游戏、上次目录、封面目录、推荐配置 JSON 路径。
  - Input / Controls：触控布局、手柄映射入口占位。
  - Diagnostics：复制全局设置/GPU 诊断。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.54 MB，时间：2026-08-16 22:23:53。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 打开 `MainActivity` 和 `SettingsActivity`：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION` / `InflateException`。
  - 本轮未启动游戏。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 58% 提升到约 63%。
  - 已有：首页、属性页、驱动页、二级管理页、全局设置页五类外壳。
- UI 100% 下一步：
  1. 运行页 overlay 美化：暂停菜单、退出确认、控制显示/隐藏、HUD 设置。
  2. 输入映射/触控布局编辑页。
  3. 首页空状态/最近游戏/封面导入刷新。
  4. Save/Add-on/Shader 从 shell 变成可实际操作。

## 2026-08-16 22:32:05 +08:00 UI 100% 路线第十步：运行页 参考风格 暂停菜单 overlay
- 本轮补 UI 100% 大项之一：运行页 overlay 美化。
- 当前 `EmulationActivity` 原本已有左右侧抽屉：左侧 NxEmu Menu、右侧 Quick Settings、触控隐藏、HUD/日志/系统栏/最小化/退出等按钮；本轮在其上新增更直观的中央暂停菜单。
- 新增/修改：
  - `EmulationActivity.kt` 新增中央 `pauseMenuPanel`。
  - 顶部中间新增 `Ⅱ` 暂停菜单 handle。
  - 暂停菜单包含：
    - 继续
    - 隐藏/显示触控
    - HUD 精简/详细切换
    - 复制日志
    - 保存日志
    - 系统栏
    - 最小化
    - 退出运行（需要二次点击确认）
    - 关闭菜单
  - 新增 `PAUSE_MENU_WIDTH=760` 常量。
- 行为说明：
  - 菜单打开时隐藏左右抽屉，保持界面不互相遮挡。
  - 退出运行按钮沿用已有二次确认逻辑，避免误触直接退出游戏。
  - 不改 native boot/render/input 逻辑，只增加 Android overlay UI。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - APK 大小：16.55 MB，时间：2026-08-16 22:31:04。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 用内置 hbmenu 路径启动 `EmulationActivity` 约 5 秒：pid 存在，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 日志显示 `LoadRom accepted`，说明本轮 UI 改动没有破坏运行页启动链路。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 63% 提升到约 68%。
  - 已覆盖：首页、属性页、驱动页、二级管理页、全局设置页、运行页暂停菜单/抽屉/触控 overlay。
- UI 100% 下一步：
  1. 输入映射/触控布局编辑页。
  2. 首页空状态/最近游戏/封面导入刷新。
  3. Save/Add-on/Shader 实际操作功能。
  4. 统一导航/返回栈/过渡动画和更接近 参考项目的视觉细节。


## 2026-08-16 22:43:38 +08:00 UI 100% 路线第十一步：Input/Controls 设置页 + 首页封面刷新入口
- 用户继续要求“UI 100%”。本轮补齐 参考风格 外壳中缺失较明显的输入/触控设置页，并把首页入口做出来。
- 新增/修改：
  - 新增 `InputSettingsActivity.kt`。
  - 新增 `activity_input_settings.xml`。
  - `AndroidManifest.xml` 注册 `.InputSettingsActivity`。
  - `SettingsActivity.kt`：Input / Controls 不再是 TODO toast，改为进入输入设置页；并在独立启动 SettingsActivity 时先初始化 `GpuDriverHelper`，修复直接 ADB 打开 SettingsActivity 会因 `appContext` 未初始化崩溃的问题。
  - `activity_main.xml`：首页顶部新增“输入”和“封面”按钮。
  - `MainActivity.kt`：新增 `openInputSettings()`、`refreshCoversAndLibrary()`、`coverCountFor()`；更多菜单增加“输入/触控设置”和“刷新封面缓存”。
  - `AppPreferences.kt`：新增可持久化输入偏好：
    - `touchLayoutPreset`：紧凑/标准/大号。
    - `touchOpacity`：30%-100%。
    - `touchInitialVisible`：进入运行页是否默认显示触控。
    - `touchAutoHide`：预留自动淡出开关。
  - `EmulationActivity.kt`：运行页读取上述触控设置；触控按钮/摇杆透明度和尺寸按全局输入设置生效；session log 记录 touchPreset/touchOpacity/touchVisible。
- InputSettingsActivity 当前能力：
  - 参考风格 header + RecyclerView property list。
  - 可切换触控布局预设、按键透明度、启动显示/隐藏触控、自动淡出预留开关。
  - 检测 Android `InputDevice` 中的 GAMEPAD/JOYSTICK/DPAD 设备并列出。
  - 可复制输入诊断。
- 首页封面入口：
  - “封面”按钮刷新封面查找结果，并显示 `已刷新 x/y`。
  - 空状态提示补充 `/sdcard/ns/covers` 规则：TitleID 或游戏文件名 `.png/.jpg/.jpeg/.webp`。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
  - 构建期间按用户要求进行了 30 秒级日志检查，未空等。
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 已解锁后依次打开：
    - `MainActivity`
    - `SettingsActivity`
    - `InputSettingsActivity`
  - 三个 Activity 均 pid 存在，无 `AndroidRuntime/FATAL EXCEPTION` / `InflateException` / `Unable to start activity`。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前进度变化：
  - 参考风格 UI 从约 68% 提升到约 74%。
  - 外壳已覆盖：首页、属性页、驱动管理、全局设置、输入设置、游戏二级管理、运行页暂停菜单/抽屉/触控 overlay。
- UI 100% 剩余大项：
  1. 首页继续 参考项目 化：最近游戏区域、空状态卡片更精致、封面导入/目录授权、真实标题图标提取。
  2. Save/Add-on/Shader 二级页从 shell 变成可导入/导出/安装/启用。
  3. 输入页后续做真正拖拽式控制布局编辑，并让每游戏覆盖触控布局。
  4. 统一导航/过渡/返回栈和视觉细节，再向 参考项目 Fragment/Navigation 靠近。
  5. UI 外壳到 85%+ 后继续回到 NCE 性能、HLE/NVDRV/VI 日志降噪、驱动兼容。


## 2026-08-16 23:02:54 +08:00 UI 方向修正：从“功能像参考项目”改为“视觉先贴 参考项目”
- 用户反馈：“UI 跟 参考项目 完全不像”。本轮停止继续堆二级功能，转为直接对照本地 参考项目 Android 资源做视觉还原。
- 参考文件：
  - `D:/project/switch-emulator-refs/参考项目/src/android/app/src/main/res/layout/fragment_games.xml`
  - `card_game_grid.xml`
  - `card_game_list.xml`
  - `drawable/参考项目_background_gradient.xml`
  - `drawable/参考项目_card_background.xml`
  - `values/yuzu_colors.xml`
- 参考项目 视觉结论：
  - 不是深色 UI，而是浅色白/浅灰 synthwave 风格。
  - 重点是白色背景、浅灰卡片、黑色文本、洋红 `#FF0080`、青色 `#60D1F6`。
  - 首页是顶部标题 + 圆形 icon 按钮 + 48dp 胶囊搜索框 + 游戏封面墙，而不是大块深色 hero 面板。
- 本轮修改：
  - 新增/覆盖 `res/values/colors.xml`，引入 参考项目 主要颜色。
  - 覆盖 NxEmu 当前 参考项目 drawables：
    - `参考项目_background_gradient.xml` 改为白底 + 轻微洋红/青色径向氛围。
    - `参考项目_game_card.xml` 改为浅灰卡片 + 16dp 圆角 + 灰边框。
    - `参考项目_game_card_selected.xml` 改为浅色选中态 + 洋红边框。
    - `参考项目_panel.xml` / `参考项目_hero.xml` / `参考项目_button.xml` / `参考项目_button_primary.xml` / `参考项目_search_bg.xml` / `参考项目_badge.xml` 改为 参考项目 浅色调。
  - `activity_main.xml`：去掉深色 hero 大块感，根布局改白色基调，顶部边距/搜索框更接近 参考项目。
  - `item_game_card.xml`：封面尺寸改接近 参考项目 grid 的 150dp，文本改黑/灰，卡片 padding/margin 调整。
  - `MainActivity.kt`：Grid 默认列数逻辑改为更接近封面墙；Grid/Compact 隐藏详情和按钮，List 保留详情和操作。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - `MainActivity`、`SettingsActivity`、`PerGameSettingsActivity` 均可启动，无 `AndroidRuntime/FATAL EXCEPTION` / `InflateException` / `Unable to start activity`。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 当前 UI 进度重新口径：
  - 功能覆盖仍约 74%，但“像 参考项目的视觉”此前不足；本轮视觉相似度从低水平提升到约 35%。
- 下一步必须继续视觉还原：
  1. 把首页顶部按钮从文字 Button 改为 参考项目的圆形 icon card（eye/filter/settings/add/folder）。
  2. 搜索框加入左侧搜索图标、右侧清空图标，按钮迁移到底部 FAB/更多菜单。
  3. 游戏卡片去掉 NxEmu 自定义启动/属性按钮常驻，改为点按/长按/菜单方式；封面墙只显示图标+标题。
  4. 设置页、属性页从深色 hero 统一改为 参考项目 浅色 Material3-like list/card。
  5. 后续如要更像参考项目，需要引入或手写简化版 VectorDrawable 图标和 Material-like card 组件。


## 2026-08-16 23:09:59 +08:00 触控摇杆放大：解决手指滑出感应圈/操作圈
- 用户反馈：左右摇杆操作容易丢失，因为玻璃上没有范围感，手指容易划出感应圈和操作圈，要求内外圈都放大。
- 修改文件：`src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
- 修改内容：
  - `ANALOG_STICK_SIZE` 从 `340` 增大到 `430`，左右摇杆 View 的可触摸区域同步增大。
  - 摇杆外圈绘制 radius 从 `half - 8` 改为 `half - 4`，可视外圈更大。
  - 外圈线宽从 `5f` 改为 `8f`，更容易看清边界。
  - 内圈/knob 半径从 `radius * 0.38` 改为 `radius * 0.44`，内圈更大。
  - knob 移动比例从 `0.62` 改为 `0.56`，减少视觉上贴边，降低误以为已出圈的感觉。
  - label 字号从 `34f` 改为 `40f`。
  - analog deadzone 从 `0.12f` 降到 `0.08f`，轻微移动更容易响应。
  - 为避免放大后重叠：
    - L3/R3 更外移。
    - D-pad 和 ABXY 面板上移。
- 构建验证：
  - `:app:assembleDebug` 通过。
  - APK：`D:/project/_nxemu_src/src/android/app/build/outputs/apk/debug/app-debug.apk`
- 真机短测：
  - 已安装到 realme RMX3700 / b72182d。
  - 启动 `EmulationActivity` + hbmenu 轻触 5 秒。
  - 日志显示 `LoadRom accepted`，无 `AndroidRuntime/FATAL EXCEPTION`。
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 后续如果用户仍觉得摇杆丢：
  1. 再把 `ANALOG_STICK_SIZE` 提到 `480`。
  2. 或做“不可见超大触摸热区 + 可见摇杆圈较小”的分离设计。
  3. 或增加触摸设置页里的摇杆单独倍率选项，而不是所有控件统一 preset。

## 2026-08-17 Kirby 反复闪退定位与临时保护

- 用户反馈“刚刚的游戏老闪退”，复测对象：`/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp`。
- 结论：不是 DNSP 路径/文件问题，也不是单纯驱动问题；当前触发点是 **Android NCE direct-map**。
- 对照测试：
  - NCE=true：约 5 秒进程消失，logcat：`Process org.nxemu.app.debug ... has died: fg TOP`，`Zygote: Process ... exited due to signal 11 (Segmentation fault)`；退出前有大量 `NxEmuNCEHeap` / `NxEmuNCEHost` direct `mmap ok` 日志。
  - NCE=false：25 秒探针仍在前台，未见 crash；session log：`nceRequested=false`、`nceEnabled=false`、`cpuBackendActual=Dynarmic`、`boot=LoadRom accepted`、`result=started`。
- 已修改 `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`：
  - `applyPerGameCompatibilityProfile()` 改到读取 `preferNce` 之后执行。
  - 增加 Kirby/TitleID `01007E3006DDA000` 兼容保护：如果用户不是通过显式 intent 强制 NCE，则自动 `preferNce=false`，防止普通启动闪退。
  - session log 会记录：`kirby: NCE auto-disabled; current Android direct-map NCE crashes this title with SIGSEGV` 或 explicit 状态。
- 构建成功：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`。
- 已安装到 realme RMX3700 并测试：`D:\project\_nxemu_src\diagnostics\kirby_probe_autonceoff_20260817-230821`。
- 后续：继续修真正 NCE；当前保护只是保证 Kirby/同类游戏先不闪退，性能会退回 Dynarmic，因此帧率会低。

## 2026-08-17 参考项目 UI 外壳继续贴近

- 用户反馈：UI 跟 参考项目 差很多。
- 原因复盘：之前只是用了 参考项目 色系/卡片背景，但结构仍是“传统安卓大按钮横排 + 诊断面板”，没有按 参考项目的 `fragment_games.xml` 结构走。
- 本轮参考：`D:\project\switch-emulator-refs\参考项目\src\android\app\src\main\res\layout\fragment_games.xml` 和 `card_game_grid.xml`。
- 已修改首页 `activity_main.xml`：
  - 根布局改成 参考风格 `FrameLayout`，支持底部 FAB overlay。
  - 顶部改为左侧大标题 `参考项目 / NxEmu Android`，右侧 42dp 圆形 icon 按钮（视图、排序、GPU、设置、更多）。
  - 搜索区改为单独的 48dp 胶囊搜索框，左侧搜索图标，右侧刷新封面小圆按钮。
  - 游戏库区域改成 参考项目的空状态/Recycler overlay 结构。
  - 扫描、文件夹、输入、属性、启动改到底部 FAB 风格按钮；启动使用洋红主按钮。
  - 诊断区默认隐藏，不再抢首页视觉。
- 已修改 `MainActivity.kt`：
  - 首页启动时隐藏系统导航/状态栏，避免右侧灰色 Android 导航条破坏 参考项目 全屏观感。
  - 顶部视图/排序/更多按钮文字改成 icon-like 简短符号。
  - 修复游戏封面尺寸：原来代码把 `150/118/66` 当裸像素，现在改成 `dp(150/118/66)`，高 DPI 手机上封面墙尺寸更接近 参考项目。
- 已新增/调整 style/color：`NxEmu参考项目IconButton`、`NxEmu参考项目IconButtonSmall`、`NxEmu参考项目FabPrimary`、`NxEmu参考项目FabSecondary`、`参考项目_on_surface_variant`。
- 构建成功，APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`。
- 真机验证：已安装并截图，截图目录：`D:\project\_nxemu_src\diagnostics\ui_参考项目_home2_20260817-232353`；测试后已 force-stop 并锁屏。
- 当前 UI 仍未 100% 参考项目的原因：
  1. 没有完全迁移 参考项目的 Material3 组件、MaterialCardView、GradientBorderCardView、自定义 selector/shapeAppearance。
  2. 图标资源仍是文字/符号替代，未完整搬 参考项目的 `ic_eye/ic_filter/ic_settings/ic_search/ic_clear` 等 vector。
  3. 设置页、属性页、驱动页仍只是浅色化，还不是 参考项目的 fragment/card/list 架构。
  4. 游戏封面墙已经修正尺寸，但缺 参考项目的渐变边框选中态和真实 Material ripple/transition。
- 下一步 UI：补 vector icons + 把属性/设置/驱动页改成 参考项目 list/card，再做封面卡片 GradientBorder 效果。

## 2026-08-17 参考项目 UI：封面墙/卡片/二级页面浅色修正

- 继续用户要求“UI 100% / 跟 参考项目 差很多”。
- 已补强 参考项目 游戏封面墙：
  - `参考项目_game_card.xml` 改为 layer-list 渐变边框 + 内层浅灰卡片。
  - `参考项目_game_card_selected.xml` 改为更明显的洋红/紫/青渐变选中边框。
  - 修复 `item_game_card.xml` 中竖向封面墙模式下文字容器仍为 `width=0dp` 的 bug：新增 `game_text_container` id，并在 `MainActivity.applyModeLayout()` 中按 List/Compact/Grid 动态设置宽度。
  - Compact 封面调整为 106dp，Grid 封面调整为 138dp，适配 realme RMX3700 560dpi 横屏，避免标题被底部 FAB 或 Recycler 高度截断。
- 已补二级页面浅色 参考项目 化：
  - `item_property_card.xml`、`item_driver_card.xml`、`item_property_header.xml` 改为 `参考项目_on_background / 参考项目_on_surface_variant` 文本色。
  - `activity_driver_manager.xml`、`activity_game_management.xml`、`activity_input_settings.xml`、`activity_per_game_settings.xml`、`activity_settings.xml` 中旧的深色主题白字残留改成浅色主题黑/灰字。
  - `activity_per_game_settings.xml` 底部深色条去掉，改透明，减少与 参考项目 浅色外壳冲突。
- 构建成功，APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`。
- 真机验证：已安装、扫描 `/sdcard/ns/rom`、截图确认标题不再裁切；截图目录：`D:\project\_nxemu_src\diagnostics\ui_参考项目_cards4_20260817-233457`。
- 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。
- 下一步 UI 剩余：
  1. 正式迁移 参考项目 vector 图标，替代当前顶部文字/符号按钮。
  2. 搜索框补 clear icon 和可点击清空逻辑。
  3. 进一步复刻 Material ripple/transition/ShapeableImageView 圆角裁剪。
  4. 设置/属性/驱动页结构继续向 参考项目的 fragment/card/list 靠齐。

## 2026-08-18 00:16 UI/参考项目 外壳进展

本轮继续推进首页 参考风格 UI，并做了真机验证（realme RMX3700 / Android 13）：

- 构建修复：`buttonMore` 从 `ImageButton`/`Button` 切换后，`MainActivity.diagnosticsToggleButton` 改为通用 `View`，旧动态诊断按钮处用安全 cast，构建通过。
- 顶栏图标化：保留 vector 图标按钮：视图、排序；更多使用圆形 参考项目 风格按钮 `•••`。
- 发现并修复顶栏点击区域问题：真机 `uiautomator` 显示最后一个 toolbar child 会被测成 `[0,0][0,0]`。处理方式：
  - 顶部 action 容器从 `wrap_content` 改固定 `220dp`，避免最后按钮被排到屏幕外。
  - 顶栏收敛为 3 个稳定入口：视图、排序、更多。
  - GPU 驱动管理、全局设置、刷新封面缓存保留在“更多”菜单内，功能不丢。
- 搜索框右侧刷新按钮同样出现 bounds=0，因此隐藏；刷新封面统一走“更多 -> 刷新封面缓存”。
- 首页最终验证截图：`diagnostics/ui_final_20260818-0016.png`。
- 更多菜单验证截图：`diagnostics/ui_more_menu_ok_20260818-0024.png`。
- 最终 APK 已构建安装：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 测试结束已执行 `am force-stop org.nxemu.app.debug` 并锁屏。

当前 UI 状态：

- 首页已接近 参考项目首页结构：左上 参考项目/NxEmu Android、右上 3 个圆形入口、胶囊搜索框、封面墙、底部大按钮。
- 顶栏为了真机点击可靠性，暂未显示独立 GPU/设置按钮；二者在“更多”菜单中。
- 后续 UI 可继续：设置/驱动/属性页继续按 参考项目 fragment/card 结构重做，封面圆角裁剪，菜单样式 Material 化。

下一步建议：

1. 继续 UI：二级页面 参考项目 化、更多菜单/排序菜单样式优化。
2. 回到核心：NCE direct-map SIGSEGV，当前 Kirby 等部分 title 仍需自动禁 NCE 保稳定。
3. 游戏兼容：继续记录 per-game 配置、驱动推荐、图形缓存/驱动兼容差异。

## 2026-08-18 00:28 UI 文案去品牌化

本轮按用户要求去掉界面上直接显示的参考项目名称：

- 首页标题从参考项目名称改为 `NxEmu`，副标题改为 `Android`。
- 主要用户可见文案中，`style/like` 类描述改为 `NxEmu Android / NxEmu`：
  - `DriverManagerActivity.kt`
  - `GameManagementActivity.kt`
  - `InputSettingsActivity.kt`
  - `MainActivity.kt`
  - `PerGameSettingsActivity.kt`
  - `SettingsActivity.kt`
- 内部资源名、颜色名、style 名、Adapter 名暂未大规模重命名，避免破坏资源引用；当前仅保证用户界面/菜单/诊断中不主动显示参考项目名字。
- 构建成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`，时间 `2026-08-18 00:24:52` 左右。
- 真机已安装验证，首页截图：`diagnostics/ui_no_reference_name_20260818-0027.png`。
- 测试结束已 `am force-stop org.nxemu.app.debug` 并锁屏。

下一步：继续 UI 二级页结构完善，或者回到 NCE/性能/游戏兼容。


## 2026-08-18 22:42 自动扫描、ROM 去重、首页入口收敛

本轮回应用户反馈：启动后不显示 ROM、需要手动扫描；Kirby/Metal Dogs 等出现重复条目且其中一份可能启动回退；首页配置入口仍过多。

已完成：

- 启动自动扫描：`MainActivity.onCreate()` 在首页初始化后自动扫描 `/sdcard/ns/rom`、`/sdcard/ns/roms`，如果上次授权的是 SAF `content://` 目录且仍有持久读权限，也会合并扫描。
- 自动保留选择：如果上次选择的游戏仍在扫描结果中，保持选中；否则自动选择排序后的第一个高优先级条目。
- 扫描去重：新增 `dedupeGameEntries()`，修复 `/sdcard/ns/rom` 与 `/storage/emulated/0/ns/rom` 指向同一目录导致的重复卡片。
- 格式优先：同一游戏同时有 `.dnsp/.nsp` 或 `.dxci/.xci` 时，优先保留 `.dnsp/.dxci`，避免点到原始/错误 sibling 后回退。
- 首页入口收敛：底部隐藏“输入”“属性”按钮，仅保留主流程“扫描 / 文件夹 / 启动”；“当前游戏属性/独立配置”“全局设置”“输入/触控设置”“GPU 驱动管理”等放进右上更多菜单。
- 说明：Android 不能静默自动授予“所有文件访问”或 SAF 目录权限，系统必须让用户确认；当前策略是已授权则自动扫描，未授权则保留“文件夹/更多->授权所有文件访问”入口。

真机验证：

- 构建成功并安装：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 启动后未手动点扫描，首页已经自动显示 ROM。
- 第一屏不再重复显示 Metal Dogs/Kirby；Kirby 选中的是 `.dnsp`。
- 更多菜单可用，配置入口已进入子菜单。
- 截图：
  - `diagnostics/ui_autoscan_dedupe_20260818-2237.png`
  - `diagnostics/ui_more_config_menu_20260818-2240.png`
- 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

后续 UI 差距说明/方向：

- 目前仍是 Android 原生轻外壳 + nxemu core，不是完整迁移参考项目的 Fragment/Navigation/Material 设置架构，所以层级、动画、卡片、二级页面仍有差距。
- 下一步若继续 UI，应重做设置、驱动、输入、游戏属性为统一的二级页面/卡片列表结构，而不是只做首页外观。

## 2026-08-18 23:02 进度分析与配置中心页面

当前进度分析：

- 可用性：APK 已能自动扫描 ROM、去重、选择/启动 `.dnsp/.dxci`，并支持驱动、分辨率、跳帧、日志、触控、每游戏配置等基本入口。
- UI：首页已有浅色卡片/封面墙/胶囊搜索/右上图标入口，但和完整参考项目仍有明显差距。根因是当前仍为 Android 原生轻外壳 + nxemu core，没有完整迁移 Fragment/Navigation/Material/Settings 层级架构。
- 权限：Android 不能静默自动授权文件访问；当前策略是已有权限自动扫描，未授权则进入系统授权/SAF 目录选择。
- ROM 路径：已处理 `/sdcard` 与 `/storage/emulated/0` 别名重复，以及 `.dnsp/.nsp` sibling 优先级问题。
- 核心：NCE 仍是性能主缺口；部分游戏当前仍靠禁用 NCE 保稳定。

本轮继续 UI 结构：

- 新增 `HomeMenuActivity.kt`，把右上“更多”从系统黑色 `PopupMenu` 改成独立的 `NxEmu 配置中心` 页面。
- `MainActivity` 右上更多按钮现在进入配置中心，而不是直接弹系统菜单。
- 配置中心按卡片分组：
  - 文件与游戏库：选择文件、授权 ROM 文件夹、扫描 `/sdcard/ns/rom`、运行上次游戏。
  - 当前游戏：属性/独立配置、信息/存档/Add-on、刷新封面缓存。
  - 系统设置：全局设置、输入/触控设置、GPU 驱动管理、文件访问权限。
  - 诊断与维护：复制首页日志、返回游戏库。
- `MainActivity` 新增 `EXTRA_HOME_ACTION` 命令分发，配置中心可回到 MainActivity 执行选择文件、授权目录、扫描默认目录、运行上次、刷新封面、复制日志、文件访问权限等需要首页上下文的动作。
- Manifest 新增 `HomeMenuActivity`，并把 `MainActivity` 设为 `singleTop`，用于配置中心回传命令。

真机验证：

- 构建成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 已安装真机并打开首页，右上更多成功进入 `NxEmu 配置中心`。
- 验证截图：`diagnostics/ui_home_menu_activity_20260818-2258.png`。
- 测试后已执行 `am force-stop org.nxemu.app.debug` 并锁屏。

下一步建议：

1. 继续把 `SettingsActivity`、`InputSettingsActivity`、`DriverManagerActivity`、`PerGameSettingsActivity` 做成同一套卡片/导航视觉，减少“只有首页像”的割裂。
2. 把配置中心里的项目做成可滚动双列完整页面，并补更明显的选中态/返回态/图标。
3. 回到核心性能：继续修 NCE direct-map SIGSEGV、降低 HLE/NVDRV/VI 高频日志、做游戏加载阶段指标。

## 2026-08-18 23:07 默认隐藏原始 NSP/XCI 与配置中心压缩

本轮先做进度复盘，然后继续处理用户反馈的 ROM 误选/重复和 UI 层级问题：

- 进度判断：
  - 首页自动扫描、去重、上次游戏恢复已可用。
  - UI 已有首页与配置中心，但仍未完整迁移参考项目的 Fragment/Material 设置架构；后续要继续改二级页。
  - NCE/性能仍是核心未完成项，当前仍以稳定运行 DNSP/DXCI 为优先。
- ROM 策略更新：
  - 新增 `AppPreferences.showRawSwitchContainers`，默认 `false`。
  - 首页/SAF 扫描默认隐藏 `.nsp/.xci`，只显示 `.dnsp/.dxci/.nro/.nca` 等更适合当前链路的文件，避免 Kirby 等同名 sibling 中误点到原始容器后回退/黑屏。
  - 手动文件选择仍保留原始容器能力，便于后续调试；如确实要显示原始容器，可在“配置中心 -> 文件与游戏库 -> 原始 NSP/XCI”或“全局设置 -> 显示原始 NSP/XCI”打开。
- UI 更新：
  - 配置中心顶部当前游戏摘要增加 原始NSP/XCI=隐藏/显示 状态。
  - 配置中心新增“原始 NSP/XCI：隐藏/显示”可点击开关。
  - 压缩配置中心卡片间距、字体和 padding，让一屏显示更多二级入口，减少之前卡片过大导致需要大量滚动的问题。
- 构建验证：
  - `:app:assembleDebug` 成功，最新 APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
  - 已安装真机，启动 5 秒探针未崩；首页自动扫描显示 5 个文件，状态为 `原始NSP/XCI=隐藏`，Kirby 只显示 `.dnsp`，不再显示 `.nsp` sibling。
  - 配置中心可打开，截图/窗口 XML：
    - diagnostics/ui_last_window.xml
    - diagnostics/ui_config_center_raw_toggle.xml
    - diagnostics/ui_config_center_raw_toggle.png
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

下一步：继续 UI 二级页统一化（Settings/Input/Driver/PerGame），再回到 NCE direct-map/SIGSEGV 和性能优化。

## 2026-08-18 23:12 二级设置页双列卡片化

本轮继续 UI 外壳，重点缩小“首页/配置中心像，但二级页不像”的割裂：

- SettingsActivity：
  - 从单列 RecyclerView 改为 2 列 GridLayoutManager。
  - 分组 Header 跨 2 列，具体设置项以两列卡片显示，更接近参考项目的二级配置页密度。
  - 顶部 hero 区、徽章和按钮高度压缩，减少纵向浪费。
- InputSettingsActivity：
  - 同样改为 2 列卡片布局，Header 跨列。
  - 顶部布局压缩，方便触控/手柄项一屏显示更多内容。
- PerGameSettingsActivity：
  - 游戏属性/独立配置页改为 2 列卡片布局，Header 跨列。
  - 后续每游戏配置项（分辨率、跳帧、NCE、驱动、日志等）会更接近参考项目的属性页结构。
- 共享卡片资源：
  - item_property_card.xml 缩小字体、padding、detail 宽度，适配双列。
  - item_property_header.xml 缩小间距，减少滚动长度。

构建与真机验证：

- `:app:assembleDebug` 成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 已安装真机并 5 秒探针测试。
- 从首页进入配置中心，再进入全局设置页成功；设置页已显示双列卡片：分辨率/画面比例、跳帧/图形兼容等并排。
- 验证文件：
  - diagnostics/ui_settings_page_grid_20260818.xml
  - diagnostics/ui_settings_page_grid_20260818.png
- 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

下一步 UI：继续把 DriverManager/PerGame 的顶部和底栏再压缩、增加图标/选中态/返回态，并继续向参考项目的 Settings 子页面层级靠齐；随后回到 NCE/性能。

## 2026-08-18 23:23 Driver/PerGame 页面继续压缩统一

本轮继续 UI 统一，补齐上一轮没有覆盖到的驱动页和游戏属性页外观密度：

- DriverManagerActivity：
  - 横屏宽度足够时驱动列表由 2 列提升到 3 列，驱动候选一屏显示更多。
  - 驱动卡片压缩 radio 标记、字体、padding 和来源路径字号，减少列表滚动。
  - 驱动页顶部操作从多行竖排改为单行：安装ZIP / 自动 / 返回 / 更多，明显降低 header 高度。
  - 状态日志区高度压缩，仍保留诊断文本用于复制/排查。
- `activity_driver_manager.xml`：压缩顶部 GPU badge、标题、摘要、按钮和 status view。
- PerGameSettingsActivity 布局：
  - 游戏封面/徽章、标题、路径、摘要、返回/重置按钮压缩。
  - 底部栏高度从 76dp 降到 60dp，快捷按钮和启动按钮缩小，给属性卡片更多空间。
- 构建验证：
  - `:app:assembleDebug` 成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 真机轻测：
  - 已安装并从首页进入配置中心，再进入 GPU 驱动管理页。
  - 驱动页打开正常，顶部按钮单行可见，驱动候选以 3 列显示。
  - 验证文件：
    - diagnostics/ui_driver_compact2_20260818.xml
    - diagnostics/ui_driver_compact2_20260818.png
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

注意：本轮测试脚本里曾用 `$pid` 变量触发 PowerShell 只读变量提示，后续 adb 脚本避免使用 $pid 作为变量名，改用 `$appPid`。

下一步：可继续 UI 图标/选中态/子菜单层级，也可以切回 NCE/性能主线。

## 2026-08-18 23:28 配置中心图标/箭头/状态感增强

本轮继续 UI，主要让配置中心更像完整二级菜单而不是纯文本列表：

- HomeMenuActivity：
  - 分组标题增加左侧小 badge：文件/当前游戏/系统/诊断分别显示简短标记。
  - 每个菜单项改为横向卡片：左侧小图标、中间标题/说明、右侧箭头或状态胶囊。
  - “原始 NSP/XCI”项右侧直接显示当前状态 隐藏/显示，不是普通箭头。
  - 增加 `sectionGlyph()` / `itemGlyph()` / `miniBadgeBackground()` / `smallIconBackground()` / `trailingPillBackground()`，统一菜单图标与状态样式。
- 构建验证：
  - `:app:assembleDebug` 成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- 真机轻测：
  - 已安装并启动首页，进入配置中心正常。
  - 配置中心可见左侧图标、分组 badge、右侧箭头/状态胶囊。
  - 验证文件：
    - diagnostics/ui_home_menu_icons_20260818.xml
    - diagnostics/ui_home_menu_icons_20260818.png
  - 测试后已 `am force-stop org.nxemu.app.debug` 并锁屏。

观察：右侧窄列部分箭头在 XML bounds 中贴近屏幕边缘，视觉上可用；后续如果继续 UI，可把配置中心从手写 LinearLayout 改成 Recycler/Grid + 统一 item view，会更接近参考项目并更容易处理响应式宽度。

下一步：建议开始切回 NCE/性能主线；UI 若继续，则做 Recycler 化配置中心、统一图标资源、ripple/selected state。

---

## 2026-08-18 23:58 Kirby/星之卡比加载回退修复

用户反馈：`星之卡比还是加载回退了`。

本轮定位：
- 旧手机偏好/每游戏配置中 Kirby 可能保存了 `prefer_nce=true`，首页启动会把该值作为 `EXTRA_PREFER_NCE` 传给 `EmulationActivity`。
- 当前 Android NCE 仍不是可玩默认路径；Kirby 显式请求 NCE 时容易表现为启动后回退/不可见。
- 复测时还抓到一次真实 native crash：`SIGSEGV addr=0x8`，符号为 `Service::HID::ControllerBase::IsControllerActivated()`，线程 `HostTiming`，发生在 `LoadRom accepted` 后；这会让用户看到“加载一下就回菜单”。

本轮修改：
1. `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
   - Kirby/星之卡比/TitleID 命中时，不再尊重显式 NCE extra 或保存 profile，强制 `preferNce=false`。
   - 日志输出：`kirby: forced NCE off for Android stability; saved/profile NCE request ignored`。
2. `src/android/app/src/main/java/org/nxemu/app/AppPreferences.kt`
   - `perGameProfile()` 对 Kirby 返回给 UI/启动链路的 profile 强制 `preferNce=false`，避免旧保存偏好继续污染首页启动。
3. `src/android/app/src/main/java/org/nxemu/app/GameProfileCatalog.kt`
   - 内置 Kirby 推荐配置改为 `preferNce=false`。
   - 增加 `migrateBuiltinProfiles()`，已有 `/sdcard/ns/config/nxemu_game_profiles.json` 中内置 Kirby profile 若仍为 NCE=true，会自动迁移为 false。
4. `src/yuzu_hid_core/resources/controller_base.cpp`
   - `IsControllerActivated()` 增加 Android 实测崩溃防御：空/stale callback 视为 inactive，避免 HostTiming HID 更新时因空 this 在 offset 0x8 崩溃。

构建/安装：
- 构建命令：Gradle `:app:assembleDebug --stacktrace --no-daemon`。
- 构建成功，APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`，时间约 2026-08-18 23:54。
- 已安装到 realme RMX3700。

真机验证：
- 测试命令显式传了 `EXTRA_PREFER_NCE=true` 作为压力场景，但代码成功强制关闭。
- 5 秒探针：Activity 保持前台，未回退，PID 存活。
- 延长到约 90 秒：未崩溃，日志持续有 Vulkan present/FPS。
- 关键日志已拉取：`D:\project\_nxemu_src\phone-logs\kirby_force_nce_off_20260818.txt`。
- 关键结果：
  - `perGameProfile=kirby: forced NCE off for Android stability; saved/profile NCE request ignored`
  - `requestedPreferNce=false`
  - `effectivePreferNce=false`
  - `nceRequested=false`
  - `nceEnabled=false`
  - `cpuBackendActual=Dynarmic`
  - `LoadRom accepted`
  - 约 9 秒后 `vulkanPresentCount=113`，`derivedPresentFps=59.3385`，`speedPercent=98.8975`
  - 约 14~60 秒多次维持接近 60FPS/100% speed，后段有 40~50FPS 波动。

当前结论：
- Kirby 这次“加载回退”的主因已经从 NCE/旧 profile 污染 + HID HostTiming 空指针崩溃两侧处理。
- 当前给用户测试的稳定包仍是 Dynarmic 路线，不是 NCE 真开；NCE 继续作为单独实验任务推进。
- 测试结束已执行 `am force-stop org.nxemu.app.debug` 并锁屏。

记录时间：2026-08-18 23:58:39 +08:00

---

## 2026-08-19 当前总进度 / 接管摘要

用户要求：检查当前所有工程进度，并把当前任务进度和必要信息回写文档，然后继续剩余任务。

当前工程状态：
- 主源码：`D:\project\_nxemu_src`
- Android 工程：`D:\project\_nxemu_src\src\android`
- 最新 APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
- 最新 APK 时间：2026-08-18 23:54:25，大小约 17.4 MB。
- 真机：realme RMX3700 / Android 13 / serial=`b72182d`。
- ADB：`D:\project\qwenasr\mobile\android-webview\.tools\android-sdk\platform-tools\adb.exe`
- 当前手机状态：已连接、app 未运行、屏幕锁定。
- 最新安装版本：`0.5.0-312-47bc919-271-g6649705-dirty-debug`，lastUpdateTime=2026-08-18 23:55:21。

当前粗略进度：
- Android UI 外壳：约 70%。首页、配置中心、设置页、输入页、驱动页、每游戏配置页、运行页 overlay 已有；但离 Eden/参考项目完整 Material/Fragment/Navigation 架构仍有差距。
- ROM 加载链路：约 65%。`.dnsp/.dxci` 主链路可用，首页自动扫描、去重、默认隐藏 `.nsp/.xci` 已完成；部分游戏仍需每游戏兼容配置。
- GPU 驱动管理：约 65%。支持外置驱动、ZIP/解压驱动、记录上次选择、每游戏 driverHint；Kirby 当前建议 Turnip 26.2。
- 运行稳定性：约 55%。Dynarmic 路线能跑部分游戏；Kirby 最新回退已修；仍需继续处理其他 title 的黑屏/慢/渲染兼容。
- NCE/性能：约 25%~35%。NCE 仍是最大性能缺口，当前不能作为普通用户默认可玩路径；应继续作为实验开关/ADB 探针单独推进。
- 整体可用度：约 55%~60%。已经能扫描、选择、启动并跑部分 `.dnsp/.dxci`，但距离稳定玩多数游戏还需 NCE、渲染兼容、驱动数据库和 UI 完整度。

最近已完成：
- Kirby/星之卡比加载回退修复：
  - 强制 Kirby 忽略旧 profile/intent 中的 NCE 请求，走 Dynarmic 稳定路径。
  - 修复 `Service::HID::ControllerBase::IsControllerActivated()` 在 HostTiming 线程 `SIGSEGV addr=0x8` 的崩溃防御。
  - 真机 5 秒探针未回退，延长约 90 秒未崩，Vulkan present/FPS 正常。
  - 日志：`D:\project\_nxemu_src\phone-logs\kirby_force_nce_off_20260818.txt`。

当前最重要未完成项：
1. NCE 继续修复：direct-map / guest entry / signal handler / TLS / TPIDRRO 仍是主线，不应污染稳定包。
2. 稳定包默认策略：普通用户包不应默认请求 NCE；否则非 Kirby 游戏可能继续出现启动回退/闪退。
3. 性能优化：Dynarmic 路线慢，需要继续减少 HLE/NVDRV/VI 高频日志、优化渲染/同步开销。
4. UI 继续向 Eden/参考项目靠近：二级页仍不够像，后续可做 Recycler/Material 化、统一图标/ripple/选中态。
5. 每游戏兼容配置库：记录 driver、分辨率、graphicsCompat、是否禁 NCE、目标 FPS 等。
6. 驱动兼容：Kirby 目前倾向 Turnip 26.2；26.3/T25/MK-I 对不同游戏需分别记录。

本轮接下来执行：
- 优先做“稳定包整理”：把全局/默认 NCE 从容易误开改为稳定默认关闭，NCE 继续保留为手动/实验开关，避免 Kirby 以外游戏因默认 NCE 请求回退。
- 构建验证后再继续 NCE 专项。

记录时间：2026-08-19 00:15:19 +08:00

---

## 2026-08-19 稳定包 NCE 默认策略收敛

本轮目标：继续剩余任务中的“稳定包整理”，避免普通用户包默认请求 Android NCE，导致 Kirby 以外的游戏也可能出现启动回退/闪退。

修改内容：
- `src/android/app/src/main/java/org/nxemu/app/AppPreferences.kt`
  - 全局 `preferNce()` 默认值从 `true` 改为 `false`。
  - 新增一次性稳定默认迁移：`stable_defaults_version=2`。
  - 首次进入新版时会把全局 `prefer_nce` 和所有已保存的 `*_prefer_nce` 每游戏配置重置为 `false`。
  - 用户后续仍可手动重新打开 NCE；迁移只执行一次。
- `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
  - `preferNce` 初始值改为 `false`。
  - `EXTRA_PREFER_NCE` 缺省读取值改为 `false`。
- `src/android/app/src/main/java/org/nxemu/app/GameProfileCatalog.kt`
  - 内置 `metal-dogs-stable` 推荐配置从 `preferNce=true` 改为 `false`。
  - `migrateBuiltinProfiles()` 扩展到 Metal Dogs 和 Kirby：已有 JSON 中的内置 profile 会自动把 `preferNce` 改成 false。
- `SettingsActivity.kt` / `PerGameSettingsActivity.kt`
  - UI 文案从“默认开启”改为“实验功能，稳定包默认关闭”。

构建验证：
- 命令：Gradle `:app:assembleDebug --stacktrace --no-daemon`
- 结果：成功。
- APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
- APK 时间：2026-08-19 00:18:50，大小约 17.4 MB。

真机验证：
- 已安装到 realme RMX3700。
- 首页 5 秒轻触测试：`MainActivity` 保持前台，未崩溃。
- 已检查 SharedPreferences：
  - `prefer_nce=false`
  - `stable_defaults_version=2`
  - 多个 `game_*_prefer_nce=false`
- logcat 关键状态：
  - `nceRequested=false`
  - `nceEnabled=false`
  - `cpuBackendActual=Dynarmic`
- 测试结束已 `am force-stop org.nxemu.app.debug` 并锁屏。

当前影响：
- 稳定包默认不会再因为旧全局/每游戏 NCE 配置误入实验路径。
- 性能会继续受 Dynarmic 限制；NCE 仍需作为单独实验主线继续修。

下一步建议：
1. 继续 NCE 专项：从 direct-map / guest entry / signal handler / TLS/TPIDRRO 继续定位。
2. 或继续 UI：二级页 Material/Recycler 化，进一步接近 Eden/参考项目。
3. 或继续游戏兼容：逐个建立 Kirby/Metal Dogs/Mystic Gate/DQB2/Zelda 的 per-game profile。

记录时间：2026-08-19 00:19:35 +08:00

---

## 2026-08-19 NCE halt bitmask 修复 / Metal Dogs stall 改善

本轮用户要求继续剩余任务，优先推进 Android NCE/性能稳定性。

复现与对照：
- 先用旧 APK/旧逻辑跑 Metal Dogs NCE：
  - 日志目录：phone-logs/probe-script-20260819-003128
  - 命令核心：-Nce 1 -InputSequence "A@8,A@10,A@10,A@10" -MaxWaitSec 120
  - 结果：约 55 秒后 renderStall=detected，vulkanPresentCount/vulkanCompositeCount 停在 2685，HUD 变成 FPS -- / Speed --。
- 同输入流程 Dynarmic 对照：
  - 日志目录：phone-logs/probe-script-20260819-003249
  - 结果：120 秒未停，vulkanPresentCount 持续增长到约 8356，说明不是 ROM/输入流程本身问题，而是 NCE halt/scheduler 路径问题。

本轮修改：
1. src/nxemu-os/core/hle/kernel/physical_core.cpp
   - 将 CpuHaltReason 判断从“枚举等值判断”改为 bitmask 判断。
   - 原因：NCE SVC trampoline 会把 SupervisorCall 与异步 BreakLoop 等 esr_el1 pending bit 用 OR 合并返回；等值判断容易漏掉组合 halt reason。
   - 逻辑现在更接近 Eden/yuzu：SupervisorCall、BreakLoop、PrefetchAbort、DataAbort、InstructionBreakpoint、StepThread 分别按位检查，并让 step completed 优先。
2. src/android/scripts/adb-probe-nxemu.ps1
   - 新增 -NceTrace 参数。
   - 默认仍为 0，需要深抓 NCE dispatch/SignalInterrupt 时可传 -NceTrace 1，脚本会设置 debug.nxemu.nce.trace。

构建：
- 命令：Gradle :app:assembleDebug --stacktrace --no-daemon。
- 本轮构建有人工轮询 CPU/日志，23 秒完成，无异常卡死。
- APK：src/android/app/build/outputs/apk/debug/app-debug.apk
- APK 时间：2026-08-19 00:40:03
- APK 大小：17,397,563 bytes。

真机验证：
- 设备：realme RMX3700 / Android 13 / b72182d。
- 已安装新 APK。
- NCE 复测 1：
  - 日志目录：phone-logs/probe-script-20260819-004027
  - 输入：A@8,A@10,A@10,A@10
  - 结果：STOP_REASON=max-wait reached 120s，app 存活。
  - 关键状态：nceEnabled=true，cpuBackendActual=NCE，vulkanPresentCount 持续增长到 8968。
  - HUD/日志速度：后段约 59~60 FPS、99~101% speed，未再出现 renderStall。
- NCE 复测 2：
  - 日志目录：phone-logs/probe-script-20260819-004353
  - 输入：A@8,DOWN@4,DOWN@1,A@1,A@10
  - 结果：STOP_REASON=max-wait reached 100s，app 存活。
  - 关键状态：nceEnabled=true，cpuBackendActual=NCE，vulkanPresentCount 持续增长到 6796，速度约 99~101%。

当前结论：
- 旧的 Metal Dogs NCE present/composite 停止增长问题，本轮 bitmask 修复后已经在标题/菜单长探针中明显改善。
- NCE 现在不是“完全不可用”，Metal Dogs 标题/菜单场景可 100~120 秒稳定满速。
- 稳定用户包策略仍建议默认关闭 NCE，因为 Kirby 等游戏仍需按 title 做白名单/黑名单验证；但 Metal Dogs 可作为 NCE 白名单候选继续做正式游戏内容验证。

后续建议：
1. 用更准确的输入序列或用户手动测试 Metal Dogs 进入正式游戏内容，验证 NCE 在战斗/剧情/读档后是否仍稳定。
2. 对 Kirby/DQB2/Zelda/Mystic Gate 分别做 NCE 短探针，建立每游戏 NCE 推荐值。
3. 若再次出现 stall，用新脚本参数 -NceTrace 1 抓 dispatch/SignalInterrupt 详细日志。
4. 继续 UI 外壳 Eden 化和每游戏配置库；稳定包仍默认 prefer_nce=false。

测试结束状态：已 am force-stop org.nxemu.app.debug 并锁屏。

记录时间：2026-08-19 00:47:48 +08:00
---

## 2026-08-19 Android HID Player1/Handheld 镜像与 NCE 正式内容探针

本轮用户要求：继续剩余任务，重点解决 Metal Dogs 进入正式内容前后输入/方向/摇杆链路，并继续观察 NCE。

修改内容：
- `src/android/native/nxemu_android_jni.cpp`
  - Android 触控/调试输入从只写 Player1 virtual port 0，改为在 `player_index==0` 时同时镜像到 Handheld virtual port 8。
  - 启动时强制准备 Player1 Fullkey，并额外准备 Handheld 控制器状态，避免手机/Handheld mode 下部分游戏只看 handheld npad。
  - `setPlayerButton()` 输出新增 `mirroredHandheld`、`hidHandheldButtonValue`。
  - `setPlayerAnalog()` 输出新增 Player1/Handheld 左右摇杆值，方便判断上下/左右是否真的写入 virtual gamepad。
  - `runtimeStatus()` 追加 Player1/Handheld stick 状态诊断。

构建：
- 命令：`Gradle :app:assembleDebug --stacktrace --no-daemon`。
- 首次构建因直接调用 `IEmulatedController::GetNpadButtons/GetSticks` 不在接口中失败，已改为接口已有的 `GetButtonsStatus/GetSticksValues`。
- 二次构建成功，用时约 20 秒，无遗留 Gradle/Ninja 进程。
- APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
- APK 时间：2026-08-19 01:12:05，大小 17,399,003 bytes。

真机测试：
- 设备：realme RMX3700 / Android 13 / serial=`b72182d`。
- 测试 1：`phone-logs/probe-script-20260819-011300`
  - 安装新 APK，Metal Dogs，NCE=1，Perf HUD 详细，GraphicsCompat=1，1/2X，Stretch。
  - 输入链路日志确认：A/DOWN/左摇杆均写入 Player1 和 Handheld，`hidHandheldButtonValue=true`，`hidHandheldStickL=0,-1`。
  - 画面停在标题页，判断可能是输入发得偏早/标题还未进入可接收窗口，作为诊断保留。
- 测试 2：`phone-logs/probe-script-20260819-011903`
  - 延后输入并发送 B/A/PLUS/Y/X/A 多按钮序列。
  - 成功进入剧情/正式内容前段，截图显示日文对话框。
  - HUD：NCE 开启，约 59.5/60 FPS，Speed 99%，Temp 约 36.2℃，FB 1920x1080，Internal~960x540。
  - present/composite 持续增长，未出现 renderStall/崩溃。

当前结论：
- Android 输入链路已有进一步确认：Java -> JNI -> VirtualGamepad -> Player1/Handheld 两路都能写入。
- Metal Dogs 在 NCE 下已经能进入剧情/正式内容前段，并保持接近满速；它可以继续作为 NCE 白名单候选。
- 仍需用用户手动或更准确自动输入确认菜单方向/摇杆在正式菜单中的行为；第一次探针没进标题可能不是 native 输入写入失败，而是输入时机/标题状态问题。

下一步建议：
1. 继续 HID：对正式菜单/对话确认 A/B、Dpad、左摇杆、右摇杆、L/R/ZL/ZR/L3/R3 映射；如用户反馈某键反，再改 Kotlin overlay 显示/映射。
2. 继续 NCE：Metal Dogs 正式内容做 3~5 分钟稳定性；Kirby 保持 NCE 黑名单；其他游戏逐一白名单/黑名单。
3. 继续 UI：外壳和设置页继续向 Eden 架构靠近，但避免影响当前可运行链路。
4. 若再次出现标题不吃输入，优先调整自动探针输入时机，而不是直接判定 HID 失败。

测试结束状态：已执行 `am force-stop org.nxemu.app.debug`，并锁屏；当前 `screenState=SCREEN_STATE_OFF` / `interactiveState=INTERACTIVE_STATE_SLEEP`。

记录时间：2026-08-19 01:21:56 +08:00

---

## 2026-08-31 进度审计：文档对照代码后的当前状态

用户要求：先通读 `ANDROID_HANDOFF.md`，再对照当前源码，更新文档后汇报进度。本轮没有改业务代码，只做审计和文档回写。

本轮检查方法：
- 已读取完整交接文档。
- CodeGraph 在 `D:\project\_nxemu_src` 未初始化，不能使用索引；改用 PowerShell 直接检查关键文件。
- 已对照以下重点文件：
  - `src/android/app/src/main/java/org/nxemu/app/AppPreferences.kt`
  - `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/MainActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/GamePathResolver.kt`
  - `src/android/app/src/main/java/org/nxemu/app/GpuDriverHelper.kt`
  - `src/android/app/src/main/java/org/nxemu/app/DriverManagerActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/SettingsActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/InputSettingsActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/PerGameSettingsActivity.kt`
  - `src/android/app/src/main/java/org/nxemu/app/GameProfileCatalog.kt`
  - `src/android/native/nxemu_android_jni.cpp`
  - `src/nxemu-os/core/hle/kernel/physical_core.cpp`
  - `src/android/scripts/adb-probe-nxemu.ps1`

当前 APK 状态：
- 路径：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
- 时间：`2026-08-19 01:12:05`
- 大小：`17,399,003 bytes`
- 本轮未重新构建 APK，因此手机上如果仍是这个包，功能状态应对应 2026-08-19 的构建。

ADB/真机状态：
- 本轮启动了 adb server，但 `adb devices` 当前没有列出 `b72182d`；`adb -s b72182d` 返回 device not found。
- 因此本轮未做真机安装/运行/截图/锁屏测试。
- 后续真机测试仍按旧规则：先判断锁屏并解锁，测试后 `am force-stop org.nxemu.app.debug`，再 `input keyevent 26` 锁屏。

代码对照确认的当前状态：
1. NCE 默认与实际开关
   - `AppPreferences.preferNce()` 默认是 `false`，并有 `stable_defaults_version=2` 的一次性迁移，会把旧全局/每游戏 NCE 偏好重置为 false。
   - `EmulationActivity.preferNce` 初值和 `EXTRA_PREFER_NCE` 缺省值也是 false。
   - `nxemu_android_jni.cpp` 中 `kAndroidNceStabilityGuardDefault=false`，也就是说 native 层不再硬性 guard；是否启用主要由 UI/intent/profile 请求、39-bit/fastmem 等资格决定。
   - 文档早期“guard=true/默认保护关闭 NCE”的描述已经过时；当前准确描述应为：稳定包偏好默认不请求 NCE，但手动/ADB 请求 NCE 时 native 层可以进入 NCE 条件判断。
2. NCE 已有关键修复
   - `physical_core.cpp` 已把 `CpuHaltReason` 改为 bitmask 判断，避免 NCE SVC/BreakLoop 组合 halt reason 被漏判。
   - `nxemu_android_jni.cpp` 保留 NCE 诊断：`nceRequested / nceNativeGuard / nceEligible39Bit / nceFastmem / nceEnabled / cpuBackendActual`。
   - `EmulationActivity.kt` 注释显示已移除额外 Java `NxEmuPerfSampler` 后台线程，只保留 UI 主线程 perfTicker，目的是规避 NCE 与 ART/JIT signal chain 冲突。
3. ROM/路径链路
   - `GamePathResolver` 支持 `.dxci/.dnsp/.xci/.nsp/.nro/.nca`，默认 roots 包含 `/sdcard/ns/rom`、`/sdcard/ns/roms` 以及 `/storage/emulated/0` 对应路径。
   - 代码仍保留 `.nsp/.xci` 解析/选择能力，但 UI 文案和扫描策略主线聚焦 `.dnsp/.dxci`；后续应继续默认隐藏原始容器，避免用户误点更新/DLC/未转换镜像。
   - `MainActivity` 启动时会自动扫描默认 ROM 目录和已授权 SAF 目录，且保存/恢复上次游戏。
4. GPU 驱动
   - `GpuDriverHelper` 支持安装 ZIP、目录驱动、外置 `/sdcard/ns/qudong/new` 和 `/sdcard/ns/qudong` 扫描、上次驱动 selected marker 恢复、每游戏 driver source。
   - 当前驱动默认评分已经不是“优先最新 26.3”，而是明显偏向 Turnip 26.2/v306/b860e01；代码注释记录：Kirby A/B 测试中 26.2 更正常，26.3 曾出现黑屏有 FPS，T25/purple 有深蓝块，MK/eden-fix 有大块蓝/黑异常。
   - 文档旧段落中“26.3 明显优于 MK-I/应提高 26.3 优先级”的结论已经被后续代码取代；当前推荐默认是 26.2，驱动仍需要按游戏单独 profile。
5. UI 外壳
   - `MainActivity` 已是 Recycler/Grid 风格游戏库，带搜索/过滤、视图切换、排序/扫描、上次游戏、配置中心、驱动管理等入口。
   - `SettingsActivity/InputSettingsActivity/PerGameSettingsActivity/DriverManagerActivity` 都已使用 RecyclerView/Grid 或卡片化布局，属于“Eden 化外壳阶段”，但不是完整迁移 Eden 的 Fragment/Navigation/Material 架构。
   - `EmulationActivity` 运行页有全屏 SurfaceView、HUD、暂停菜单、快捷设置、触控 overlay、L/R/ZL/ZR/L3/R3、左右摇杆、菜单/齿轮折叠逻辑和隐藏系统栏逻辑。
6. 输入链路
   - `nxemu_android_jni.cpp` 已将 Android 输入镜像到 Player1 fullkey 和 Handheld npad 两路，运行状态输出包含 Player1/Handheld stick/button 诊断。
   - 仍需真机继续验证所有映射：A/B、Dpad、左右摇杆方向、L/R/ZL/ZR/L3/R3、外接手柄。
7. 日志/探针
   - `adb-probe-nxemu.ps1` 支持安装、NCE/NCE trace、Perf HUD、输入序列、自适应等待、截图间隔、StopOnPattern、最终 force-stop/锁屏（除非 `-NoFinalStop`）。
   - 探针策略仍按用户要求：先轻触短测，达到日志/崩溃/截图/进程状态目标即可停止，不机械跑满固定时间。
8. 仓库状态
   - `git status --short` 显示大量 `M` 和 `??`，包括 Android 工程、NCE、HLE/NVDRV/VI、Vulkan、shader、诊断日志等。当前仓库是本地复杂工作树，不能 reset/clean，清理前必须逐项确认。

当前进度估计（按代码审计后的口径）：
- Android UI 外壳：约 72%。已有首页/设置/输入/驱动/每游戏/运行页，但离 Eden 100% 仍差完整 Material Navigation、统一组件资产、封面/菜单细节和二级配置层级。
- ROM 加载链路：约 68%。`.dnsp/.dxci` 主线可用，自动扫描/路径修复/去重已有；仍需继续区分本体/更新/DLC、完善异常回退日志。
- GPU 驱动管理：约 72%。ZIP/目录/外置目录/上次选择/每游戏驱动已具备；驱动推荐库仍需按游戏验证。
- 运行稳定性：约 62%。Kirby 回退和 HID 崩溃已有修复，Metal Dogs 可进入内容；更多游戏还没系统覆盖。
- NCE/性能：约 45%。Metal Dogs NCE stall 已明显改善，native guard 不再强制关闭；但稳定默认仍不请求 NCE，Kirby 仍建议黑名单，其他游戏白名单/黑名单未完成。
- HID/触控输入：约 65%。两路 npad 写入已完成，UI 按键齐全；仍需实测方向、A/B、摇杆、肩键和外接手柄。
- 整体可用度：约 62%。已经不是 PoC 白屏阶段，能扫描/选择/驱动/运行部分游戏；离“多数游戏可稳定玩”仍主要卡在 NCE、驱动兼容库、渲染兼容和 UI 完整度。

当前最高优先级任务清单：
1. P0：重新连上真机后，用最新 APK 复测 Metal Dogs NCE 正式内容 3~5 分钟，确认输入/剧情/战斗/读档/切场景是否稳定。
2. P0：建立每游戏 NCE 白名单/黑名单：Kirby 默认 off；Metal Dogs 候选 on；Mystic Gate、DQB2、Zelda/其他 `.dnsp/.dxci` 待测。
3. P0：继续 NCE 专项：若某游戏卡住/崩溃，开启 `-NceTrace 1` 抓 dispatch/signal/loader/tombstone。
4. P1：驱动兼容库：默认 26.2，按游戏记录 26.2/26.3/T25/MK/v849 的画面、黑块、黑屏、FPS、卡住点。
5. P1：UI 继续 Eden 化：统一 Material 风格菜单、封面墙、二级设置、驱动页、每游戏页和运行页暂停/快捷菜单；不要只靠文字按钮。
6. P1：输入体验：实测和修正 A/B、摇杆方向/死区、肩键、外接手柄；触控布局支持更细的可编辑配置。
7. P1：性能与日志：继续压低 HLE/NVDRV/VI 高频日志，完善 FPS/Speed/目标 FPS 识别，保留 `/sdcard/ns/logs` 自动日志。

记录时间：2026-08-31 22:19:25 +08:00



---

## 2026-09-01 Kirby NCE / alternate signal stack 修复验证

本轮用户要求继续优化 NCE，重点让 Kirby 支持 NCE，并继续记录可交接状态。

关键发现：
- 对照测试确认 Kirby 非 NCE/Dynarmic 路径本身可运行：12 秒探针存活，Vulkan present 持续增长，说明 ROM/驱动/基础渲染链路不是根因。
- 旧 NCE 闪退与 Android NCE signal stack 强相关：
  - `debug.nxemu.nce.altstack=false`：Kirby NCE 启动早期直接退出，常见裸 `signal 11`/无有效 guest 画面。
  - `debug.nxemu.nce.altstack=true`：Kirby NCE 可进入标题/存档界面，30 秒探针稳定，present 持续增长。
- `llvm-readelf -l` 检查 `libnxemu-cpu.so/libnxemu-android.so/libnxemu-os.so` 的 `GNU_STACK` 均为 `RW`，不是 APK ELF 自身声明可执行栈；logcat 中的 `execstack denied` 仍需保留观察，但本轮不是阻塞原因。

本轮代码修改：
1. `src/nxemu-cpu/nce/arm_nce.cpp`
   - `UseNceAltSignalStack()` Android 默认从 `false` 改为 `true`。
   - 与 Eden/yuzu NCE 更一致：NCE signal handler 默认走 alternate signal stack。
   - 仍保留 `debug.nxemu.nce.altstack` 属性用于 A/B：设 `false` 可复现旧崩溃路径，设 `true` 为当前推荐路径。
2. `src/android/scripts/adb-probe-nxemu.ps1`
   - 新增 `-NceAltStack` 参数：`1=true`、`0=false`、`-1=不设置属性/使用 native 默认`。
3. `src/android/app/src/main/java/org/nxemu/app/GameProfileCatalog.kt`
   - 内置 Kirby/Metal Dogs 推荐配置同步为 `preferNce=true`。
   - Kirby 描述更新为 `Turnip 26.2 + NCE + alternate signal stack + 1X + 图形兼容`。
   - 迁移逻辑不再把 Kirby/Metal Dogs 旧内置 profile 强制改回 Dynarmic。

构建：
- 构建必须使用 JDK17：`D:\project\qwenasr\mobile\android-webview\.tools\jdk17\jdk-17.0.18+8`。
- 若默认 `JAVA_HOME` 指到 JDK8，会失败：Android Gradle Plugin 8.9.1 需要 JVM 11+。
- 最终构建成功：
  - APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
  - 时间：`2026-09-01 00:02:26 +08:00`
  - 大小：约 17.4 MB。

真机验证：
- 设备：realme RMX3700 / Snapdragon 7+ Gen 2 / Android 13 / serial `b72182d`。
- 最终短测：`phone-logs/probe-script-20260901-000315`
  - 安装最终 APK 成功。
  - Kirby：`/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp`
  - 参数：`Nce=1`、`NceAltStack=1`、`GraphicsCompat=1`、`FrameSkip=0`、`ResolutionSetup=0`、`AspectRatio=4`。
  - 结果：`STOP_REASON=max-wait reached 12s`，`APP_ALIVE=True`。
  - 关键状态：`nceEnabled=true`、`cpuBackendActual=NCE`、`vulkanPresentCount=541`。
- 30 秒长一点探针：`phone-logs/probe-script-20260831-235923`
  - `STOP_REASON=max-wait reached 30s`，`APP_ALIVE=True`。
  - 截图已进入 Kirby 存档选择界面。
  - HUD：约 `59.6/60 FPS`、`Speed 99%`、`CPU NCE nce=true`、`Internal~960x540`、`FB 1920x1080`。
  - `vulkanPresentCount` 增长到 2014，无 `renderStall`/SIGSEGV。
- 测试结束已由探针脚本 `am force-stop org.nxemu.app.debug` 并锁屏。

当前结论：
- Kirby NCE 已从“启动早期闪退/无法加载”推进到“可进标题/存档界面，短时满速稳定”。
- 还不能宣称 Kirby 完整通关级稳定；下一步需要 3~5 分钟进入正式关卡测试，观察切场景、shader 编译、音频同步和存档读取。
- Snapdragon 7+ Gen 2 / Adreno 725 并不算瓶颈：在 Kirby 标题/存档阶段 NCE 已接近满速；后续性能主要看 NCE 完整稳定、GPU driver、shader/pipeline cache 和具体游戏场景。

下一步建议：
1. P0：Kirby NCE 进入正式关卡做 3~5 分钟测试，确认是否仍 30/60 FPS 满速、是否有图形块/黑屏/输入卡住。
2. P0：把 NCE altstack 默认 true 后再复测 Metal Dogs，确认不会引入回归。
3. P1：继续 Eden 化 UI：首页封面墙、二级设置层级、每游戏属性页和运行中暂停菜单继续统一成 Eden/Material 风格。
4. P1：Git 推送前先整理 `.gitignore`，排除 `phone-logs/`、`diagnostics/`、build cache/APK，再创建安卓分支提交。
5. P1：官方同步必须先把当前 Android 分支提交/备份，再 fetch/merge 官方；当前工作树太脏，不能直接 merge。

记录时间：2026-09-01 00:04:08 +08:00

---

## 2026-09-01 00:11~00:14 推送与 NCE 真机复测

本轮动作：
- 已将安卓分支推送到用户 GitHub 远程：`kknd222/android-nxemu-port-20260901`。
- 推送结果：`android-nxemu-port-20260901 -> android-nxemu-port-20260901`。
- 推送前提交：`0341ef5 Add Android port with NCE altstack support`。

真机复测环境：
- 设备：realme RMX3700 / Android 13 / serial `b72182d`。
- APK：`src/android/app/build/outputs/apk/debug/app-debug.apk`。
- NCE 参数：`-Nce 1 -NceAltStack 1`。
- 图形参数：`GraphicsCompat=1`、`FrameSkip=0`、`ResolutionSetup=0`、`AspectRatio=4`。
- 测试结束后已执行 `am force-stop org.nxemu.app.debug` 并锁屏。

Kirby 60 秒探针：
- 日志目录：`phone-logs/probe-script-20260901-001113`。
- 游戏：`/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp`。
- 结果：`STOP_REASON=max-wait reached 60s`、`APP_ALIVE=True`。
- 关键状态：`nceEnabled=true`、`cpuBackendActual=NCE`。
- Vulkan present 从 239 增长到 3243，说明渲染持续出帧。
- 截图确认能显示 Kirby 标题画面；HUD 显示 NCE、Internal~960x540、FB 1920x1080。截图末尾采样约 `47/60 FPS`、`Speed 78%`，需要后续进入正式关卡长测判断是否只是标题/采样/后台波动。

Metal Dogs 45 秒探针：
- 日志目录：`phone-logs/probe-script-20260901-001237`。
- 游戏：`/sdcard/ns/rom/Metal Dogs [0100A6E01681C000][v0][JP].dnsp`。
- 结果：`STOP_REASON=max-wait reached 45s`、`APP_ALIVE=True`。
- 关键状态：`nceEnabled=true`、`cpuBackendActual=NCE`。
- Vulkan present 从 39 增长到 2727，持续出帧。
- 截图确认标题画面正常；HUD 约 `59.6/60 FPS`、`Speed 99%`。

当前结论更新：
- NCE alternate signal stack 默认 true 后没有观察到 Kirby/Metal Dogs 启动回归。
- Kirby NCE 已可持续 60 秒不崩并显示标题画面；Metal Dogs NCE 45 秒标题满速。
- 下一步应做 Kirby 正式关卡 3~5 分钟、Metal Dogs 进入正式游戏内容 3~5 分钟，验证切场景、输入、shader cache、音频同步和长期温度/降频。
- UI 仍需继续 Eden 化；当前运行页 touch overlay 和 HUD 仍偏调试状态，正式默认应精简成 `FPS / Speed / Temp`，详细 HUD 只在调试开关开启时显示。

记录时间：2026-09-01 00:16:00 +08:00

---

## 2026-09-01 00:17~00:20 Kirby 正式地图 90 秒探针

目的：
- 验证 Kirby 不只是在标题/存档页可 NCE，而是能进入正式地图/关卡选择内容。
- 验证精简 HUD 是否符合用户要求：只显示 FPS、Speed、Temp。

测试参数：
- 游戏：`/sdcard/ns/rom/KirbyStar Allies v4.0.dnsp`。
- 日志目录：`phone-logs/probe-script-20260901-001719`。
- 参数：`Nce=1`、`NceAltStack=1`、`PerfHudDetailed=0`、`GraphicsCompat=1`、`FrameSkip=0`、`ResolutionSetup=0`、`AspectRatio=4`。
- 输入序列：`A@5:200,A@8:200,A@8:200,A@8:200`。
- 探针：约 90 秒，截图间隔 15 秒。

结果：
- `STOP_REASON=max-wait reached 92s`。
- `APP_ALIVE=True`。
- 最终截图确认已经进入 Kirby 正式地图/关卡选择画面。
- 精简 HUD 显示：`FPS 29.6/30 | Speed 99% | Temp 34.3℃ batt`。

结论：
- Kirby 的 30FPS 场景里，`29.6/30 + Speed 99%` 应按满速理解；不是所有游戏都以 60FPS 为目标。
- 当前 FPS 目标识别在 Kirby 场景有效，能把目标帧率显示为 `/30`。
- NCE + altstack 在 Kirby 正式地图阶段至少 90 秒稳定，无闪退/回退。
- 后续还需要 3~5 分钟以上手动/自动长测，包括进入实际可操作关卡、切场景、shader cache 和音频同步。
- 测试命令外层 timeout 需要大于 `InitialWait + 输入延迟总和 + MaxWait + 截图/拉日志开销`，否则脚本已跑完但 shell 可能截断最后输出；后续人工执行长探针时应额外留 30~60 秒余量。

记录时间：2026-09-01 00:22:00 +08:00

---

## 2026-09-01 00:27~00:30 首页 Eden 化小步迭代

本轮 UI 改动：
- `MainActivity.kt` 游戏卡片详情增加短路径显示，例如 `路径:ns/rom/Metal Dogs ...dnsp`。
- Grid/Compact 模式下，只有当前选中的卡片展开详情和 profile，未选中卡片保持封面墙风格，减少信息噪音。
- 目的：保留 Eden 式游戏库/封面墙，同时解决重复 ROM、坏包、update/DLC 与本体混淆时看不清实际路径的问题。

验证：
- 重新构建 APK 成功，APK 时间：`2026-09-01 00:27:13 +08:00`。
- 已安装到真机并启动首页冒烟。
- 截图：`phone-logs/ui-smoke-20260901-0028/home.png`。
- 首页能显示游戏卡片、启动/属性按钮、短路径；测试结束已 force-stop 并锁屏。

后续 UI 工作：
- 当前只是低风险增量，仍未达到 Eden 完整架构。
- 继续方向：Fragment/二级设置结构、Material 式列表项、游戏详情页封面与 Metadata、运行中暂停菜单、驱动/输入/日志页统一视觉。

记录时间：2026-09-01 00:31:00 +08:00

## 2026-09-01 12:30 FPS/Speed `--` 诊断与修复

用户反馈：当前 Android 版本进入游戏后 HUD 显示 `FPS --` / `Speed --`，且手机暂时不在电脑旁，无法即时 ADB 抓日志。

本轮本地诊断结论：

- 运行页 HUD 的 FPS/Speed 主要来自 `NativeLibrary.getPerformanceStats()`。
- Android native 当前为避免 core perf stats 生命周期崩溃，使用 Vulkan present/composite counter 推导 FPS/Speed。
- 如果 `libnxemu-video.so` 中 `NxemuAndroidGetVulkanPresentCount` / `NxemuAndroidGetVulkanCompositeCount` 没有被动态符号表导出，或者 present 暂时不增长，Java 侧原逻辑会长期显示 `--`。

本轮修复：

1. `src/yuzu_video_core/renderer_vulkan/renderer_vulkan.cpp`
   - 给 `NxemuAndroidGetVulkanCompositeCount`、`NxemuAndroidGetVulkanPresentCount`、`NxemuAndroidGetVulkanLastFbWidth/Height/Stride/Format` 增加 Android default visibility + used 标记，确保 `dlsym()` 能解析到。
2. `src/android/native/nxemu_android_jni.cpp`
   - `getPerformanceStats()` 新增 `vulkanCounterSymbols=present:...,composite:...,fb:...` 输出，方便用户只靠复制日志也能判断是符号解析失败还是渲染没出帧。
3. `src/android/app/src/main/java/org/nxemu/app/EmulationActivity.kt`
   - HUD 不再只依赖 `vulkanPresentCount`。
   - 新兜底顺序：present delta -> composite delta -> native `gameFps/systemFps` -> native `derivedPresentFps/derivedCompositeFps` -> 最近非零值。
   - 如果仍无任何帧数据，显示 `采集中`，详细 HUD 显示 counter symbol 状态，避免只看到一排 `--` 无法诊断。

构建验证：

- APK 构建成功：`src/android/app/build/outputs/apk/debug/app-debug.apk`
- 构建时间：2026-09-01 12:24:08
- APK 大小：15,323,182 bytes
- 已用 `llvm-nm -D` 验证 `libnxemu-video.so` 导出：
  - `NxemuAndroidGetVulkanCompositeCount`
  - `NxemuAndroidGetVulkanPresentCount`
  - `NxemuAndroidGetVulkanLastFbWidth/Height/Stride/Format`

待用户回家/手机可用后验证：

- 安装最新 APK 后进游戏观察 HUD：应显示 FPS/Speed 数值；若还未出帧，会显示 `采集中`。
- 若仍异常，复制诊断日志，重点看 `vulkanCounterSymbols`、`vulkanPresentCount`、`vulkanCompositeCount`、`derivedPresentFps`、`derivedCompositeFps`。


## 2026-09-01 13:xx 本轮更新（SystemModules 失败细化诊断）

### 用户输入
- 用户提供最新版 copy-button 日志，版本 `0.6.0-462-65abc63-173-gb06ea2a-dirty-debug`。
- 现象：Kirby 路径存在、驱动 26.2 已安装，但 FPS/Speed 显示“采集中”，游戏未加载。

### 日志结论
- `gameExists=true`、`gameSize=4231293897`：ROM 路径和文件存在。
- `vulkanCounterSymbols=present:1,composite:1,fb:1`：上一轮 Vulkan counter 导出修复在该包中已经生效。
- `vulkanPresentCount=0`、`vulkanCompositeCount=0`：没有任何画面帧产生。
- `SystemModules setup invalid`、`modules=none_or_invalid`、`lastBootLoaded=false`：真正失败点仍是 SystemModules 初始化失败，LoadRom 没进入。
- `nceRequested=true` 但 `nceEnabled=false/cpuBackendActual=Dynarmic`：本次不是 NCE 崩溃，NCE 尚未进入。
- `input ... input=failed` 是 SystemModules 没起来后的连带结果，不是单独的输入映射问题。

### 本轮代码修复
- `src/nxemu-core/modules/module_base.h/.cpp`
  - 新增 `ModuleBase::LastLoadDiagnostic()`。
  - `ModuleBase::Load()` 记录：模块路径、期望类型、dlopen 结果、`dlerror`、`GetModuleInfo`、模块名/类型/版本、通用导出、模块专用导出、`ModuleInitialize` 返回值。
- `src/nxemu-core/modules/system_modules.h/.cpp`
  - 新增 `SystemModules::LastSetupDiagnostic()`。
  - `SystemModules::Setup()` 记录：`moduleDir`、loader/cpu/video/os 文件名、每个模块加载明细、CreateSystemLoader/CreateCpu/CreateOS/CreateVideo、各 Initialize 步骤。
  - 修正失败模块被置空后诊断丢失的问题：`LoadModule()` 现在返回失败明细文本。
- `src/android/native/nxemu_android_jni.cpp`
  - `EnsureSystemModulesReady()` 在 copy-button 日志/runtimeStatus 中输出 `systemModulesDiagnostic:`。

### 构建验证
- 已执行 `:app:assembleDebug --stacktrace`，构建成功。
- 输出 APK：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`。
- 下一次用户复制日志时，应能看到具体失败位置，例如：`loaderModule=failed` / `cpuModule=failed` / `videoModule=failed` / `osModule=failed`、`dlerror=...`、`CreateVideo=failed`、`VideoInitialize=failed` 等。

### 下一步
1. 安装本轮 APK 到手机。
2. 复现 Kirby/Mystic Gate。
3. 复制诊断日志，重点看 `systemModulesDiagnostic:` 区块。
4. 根据具体失败项继续修模块加载、符号导出、so 依赖、Video Initialize 或 OS Initialize。


## 2026-09-01 13:32 本轮热修（官方模块子目录变更导致 Android SystemModules invalid）

### 新增判断
- 对比用户日志和官方同步记录后，确认高概率回归来自官方 `#167 Modules: Update the modules so they go in to a directory instead of the root dir`。
- Android APK 的 native libs 实际仍平铺在 `nativeLibraryDir`：
  - `libnxemu-loader.so`
  - `libnxemu-cpu.so`
  - `libnxemu-video.so`
  - `libnxemu-os.so`
- 官方默认值已变成：
  - `loader/libnxemu-loader.so`
  - `cpu/libnxemu-cpu.so`
  - `video/libnxemu-video.so`
  - `operating_system/libnxemu-os.so`
- Android 运行时只覆盖了 `ModuleDirectory`，没有覆盖四个模块文件名，所以 `SystemModules::Setup()` 会在 APK nativeLibraryDir 下寻找不存在的子目录路径，最终 `SystemModules setup invalid`，游戏未进入 LoadRom。

### 本轮修复
- 修改 `src/android/native/nxemu_android_jni.cpp`：在 `EnsureRuntimeInitialized()` 设置 `ModuleDirectory=g_native_library_dir` 后，Android 强制覆盖：
  - `ModuleLoader=libnxemu-loader.so`
  - `ModuleCpu=libnxemu-cpu.so`
  - `ModuleVideo=libnxemu-video.so`
  - `ModuleOs=libnxemu-os.so`
- 保留上一条提交加入的 `systemModulesDiagnostic:` 明细日志。若还有问题，下次复制日志会继续指出具体模块/Initialize 失败项。

### 构建验证
- 新 APK 构建成功：`D:\project\_nxemu_src\src\android\app\build\outputs\apk\debug\app-debug.apk`
- 构建时间：2026-09-01 13:32:53
- APK 大小：15,328,106 bytes
- 已检查 APK 内 so，确认模块库平铺在 `lib/arm64-v8a/` 下。

### 与“之前能进游戏”的关系
- 之前能进游戏的版本是在官方模块子目录改动合并前或 Android 尚未受该默认值影响时测试的。
- 当时不是特殊 USB 参数让它能运行，主要测试参数是：真机 ADB、26.2/26.3 驱动按游戏切换、Kirby/Metal Dogs 多数场景使用 NCE+altstack，部分稳定包曾对 Kirby 强制 Dynarmic 规避 NCE crash。
- 当前这次 `SystemModules invalid` 比 NCE/驱动更早发生，属于模块路径回归；本轮 APK 应优先验证它是否恢复 `LoadRom accepted` 和 FPS 出帧。
