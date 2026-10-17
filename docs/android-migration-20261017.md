# Android 基线与定制迁移清单

核对日期：2026-10-17。本文根据实际 Git 历史、源码和仓库内交接记录整理；历史手机测试未经本次独立复现。

## 固定版本与分支

- 官方基线：`N3xoX1/nxemu`，`c8df55897359b35efa5f680d2eefbb20519a6219`。
- 用户旧 Android：`android-nxemu-port-20260901`，`87c047169f16fdcb16ddb11f8fff03e6e1e7f4cd`，保留不改。
- 用户原 master：`af7db73fea25d9c7a7c86c6eac8330e570274455`，保留不改。
- 官方快照：`upstream/android-20261017`，指向官方原始提交。
- 新 Android 工作分支：`android/upstream-20261017`，从官方基线建立。
- 旧 Android 与最新官方的共同祖先：`138fb90e4ff1775f2861e4e7dac869b3457bd5c8`（2026-08-30）。两侧各有 51 / 245 个独有历史提交；此数字包含合并和非 Android 提交，不等于功能或补丁数量。

## 判断

旧分支采用 `org.nxemu.app`、原生 Android 页面和 `src/android/native/nxemu_android_jni.cpp`；官方采用 `org.nxemu`、WebView 游戏库/设置与独立 JNI 会话、输入和设置桥。它们是不同前端和 JNI 实现，不能直接覆盖目录。

旧交接记录包括 RMX3700 上 Kirby、Metal Dogs 的 NCE 短时出帧记录，也包括后续模块路径回归及构建修复；未发现最后修复提交的完整游戏长测结果。官方已实现 NCE、驱动管理、触控/手柄、Surface 恢复及近期 CPU/渲染修正，但这些源码证据不能证明本机实际性能或稳定性更好。

选择官方新版作为维护基线，旧分支作为功能来源和行为对照。用户补充要求：优先官方，旧实现仅用于对照并修复经验证仍存在的问题；不把逐游戏手动配置作为默认使用方式。先建立可重复 APK 构建，再按下表迁移。不得把“有入口/有代码”直接当成“功能已完成”。

## 配置策略复核

旧 `GameProfileCatalog.kt` 的 Kirby 规则用 `kirby` / `星之卡比` 名称模糊匹配，未指定 Title ID；驱动通过 `26.2` 等字符串选择第一个匹配项。`AppPreferences.perGameKey` 则以文件路径的 SHA-1 为配置键。这些做法会造成不同作品误匹配、驱动构建混淆、移动文件后设置失联，且旧规则未限定核心版本和游戏 build ID。旧测试观察只能作回归用例，不能视为新版通用兼容结论。

建议采用：官方统一默认值 → 按真实设备/GPU/驱动能力判断 → 少量已验证的自动例外规则 → 可选高级手动覆盖。自动例外按 ROM 读取的 Title ID、游戏版本/build ID、GPU、驱动构建及适用核心版本限定，记录原因、验证场景和撤销条件。未命中规则时使用默认值，不推测最佳设置。退出游戏后不污染全局设置。

配置覆盖只记录必要差异；显示分辨率与画面比例等偏好不与正确性修复捆绑。修复通用内核/驱动问题优先于增加游戏规则。NCE 不可用可依据官方已有能力检查回退；不得承诺能够自动识别画面错误或任意切换运行中 CPU 后端/驱动。崩溃后若提供兼容重试，应在下一次启动进行并说明代价。

第一版不照搬旧每游戏设置页及内置规则；先验证官方默认表现，再决定是否需要自动例外。当前尚无覆盖多数游戏的验证矩阵，不能宣称“完整通用兼容”。

## 迁移清单

| 能力 | 旧分支证据 | 官方对应 | 处理与验证 | 顺序 |
|---|---|---|---|---|
| 模块、JNI、游戏启动、Surface | `src/android/native/nxemu_android_jni.cpp` | `app/src/main/jni/{native,emulation_session,android_render_window}.*` | 使用官方完整链路；验证四个模块和 JNI 库都进入 APK，手机进入游戏后才算启动通过 | P0 |
| 自定义 GPU 驱动 | `app/GpuDriverHelper.kt`、DriverManagerActivity | `utils/GpuDriverHelper.kt`、GpuDriverFetcher、libadrenotools | 沿用官方；检查 hook 库打包和实际驱动日志。每游戏驱动选择单独迁移 | P0/P1 |
| NCE / Dynarmic | 旧 altstack、诊断、游戏推荐策略 | 官方 `nxemu-cpu/nce` 与 `NXEMU_HAS_NCE` 条件构建 | 沿用官方内核，不直接搬旧 NCE 实现；在手机核实实际后端而非只看开关 | P0 |
| 每游戏配置与推荐 profile | AppPreferences、GameProfileCatalog、PerGameSettingsActivity | 官方全局系统/图形设置已有，未发现旧 profile 的等价 Android 层 | 暂不搬旧页面/规则；先官方统一默认，仅为已验证的剩余问题设计自动例外与可选高级覆盖 | 按实测需要 |
| 中文偏好与 CJK 字体修复 | `b6bd3a46`、`daff412a`、`f3b22a04`、`c8bd3457` | 需逐处比对最新语言/共享字体/更新 NACP 读取 | 按当前代码重放必要修正，检查不支持中文游戏的语言回退；不强制全局覆盖 | P1 |
| 一键诊断与运行信息 | 旧 NativeLibrary、EmulationActivity、模块诊断提交 `08b6e035` | 官方有日志和原生性能接口 | 迁移有用诊断字段和导出入口；FPS 使用官方统计，避免 present 频率冒充游戏 FPS | P1 |
| 金手指加载、选择与间隔 | `d183c710`、`e52b2655`、`6d1666cf`、`2cf6fea5`、cheat_engine | 需与新 loader/OS 内存接口逐项核对 | 先迁移后端加载，再做 Android 控制；用当前进程地址空间和 build ID 验证 | P2 |
| 更新/DLC/外置 addon 目录 | `7a466745`、GameManagementActivity | 官方 loader/补丁机制已有；Android 入口不等于完整安装管理 | 核实旧 UI 的 manifest 是否实际接入后端，迁移有效链路并检查版本选择 | P2 |
| 存档备份与恢复 | GameManagementActivity 的 save ZIP 方法 | 未发现相同 Android 管理页 | 重新适配每用户/每游戏真实存档路径；备份验证后再允许覆盖恢复 | P2 |
| Shader cache 管理 | GameManagementActivity 的 shaderDirs | 官方渲染缓存机制 | 仅迁移用户入口，按官方实际目录定位，不搬旧渲染代码 | P2 |
| Mii 与个别游戏修复 | `967fd934`、`fbc7fc7e`、`2900261a`、`bdeeb932`、纹理修正 | 官方已有大量 OS/渲染更新 | 复现同一问题后逐项迁移仍必要的补丁；避免重复修复或回退新实现 | P2 |
| 游戏库、封面、最近游戏、游玩时长与首页 | MainActivity、HomeMenuActivity、资源布局 | 官方 WebView 游戏库、元数据、主题、首次配置 | 先用官方界面；按用户需要迁移信息和操作，暂不替换整个前端 | P3 |
| 触控、手柄、暂停菜单 | 旧 EmulationActivity、InputSettingsActivity | 官方 overlay、input mapping、生命周期处理 | 使用官方实现；确认按键、摇杆、暂停/恢复和返回前台 | P0 |

## 验收与交付约束

1. 基线先构建 arm64 Release，保留官方应用 ID `org.nxemu`，与旧 `org.nxemu.app.debug` 分开安装。
2. 校验 APK 签名、ABI、JNI、四个模块及 adrenotools hook 库；保存源提交与 SHA-256。构建通过不等于手机运行通过。
3. 首次启动、驱动选择、游戏启动、触控/手柄、暂停恢复需手机实测。比较性能必须使用相同游戏、驱动、分辨率、后端和测量场景。
4. 每一批迁移独立提交并记录测试；官方快照和旧分支不做覆盖或强制推送。
5. 当前第一阶段不含旧功能迁移完成声明，也不承诺比旧版或 ScarletNX 更快。
