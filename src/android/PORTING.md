# NxEmu Android porting map

## 当前基线

NxEmu 已经具备一部分 Android/ARM64 准备：

- 根 `CMakeLists.txt`：
  - 能识别 `aarch64|arm64|ARM64`
  - Android 时跳过桌面 frontend：`if(NOT ANDROID) add_subdirectory(src/nxemu)`
- `nxemu-video`：
  - Android 链接 `log` / `android`
  - arm64-v8a 链接 `adrenotools`
  - Vulkan surface 已有 `VK_USE_PLATFORM_ANDROID_KHR` / `ANativeWindow` 路径
- `nxemu-cpu`：
  - `ARCHITECTURE_arm64` / Dynarmic ARM64 路径已有条件代码
- `nxemu-os`：
  - Android 默认构建 `NXEMU_BUILD_NXEMU_OS=ON`
  - Android 使用 OpenSSL/Unix socket 路径
- `yuzu_common/android`：
  - 已有 `java_bridge.cpp`
  - 期望 Java 侧类：`org.nxemu.input.NxemuInputDevice`
- `yuzu_input_common`：
  - 已有 Android input driver 条件路径

## 本次新增

- `src/android`：Android Gradle 工程入口。
- `src/android/native`：Android-only JNI 桥接库 `nxemu-android`。
- `src/android/app/src/main/assets/hbmenu.nro`：内置合法 homebrew 测试 NRO。
- 根 CMake Android 时构建：
  - `nxemu-android`
  - `nxemu-loader`
  - `nxemu-cpu`
  - `nxemu-video`
  - `nxemu-os`

## 参考源码对应关系

| 能力 | 主要参考 | NxEmu 落点 |
|---|---|---|
| Gradle/NDK/CMake 外壳 | Eden/Citron/Suyu `src/android` | `src/android` |
| JNI NativeLibrary API | Eden/Citron `NativeLibrary.kt` | `org.nxemu.app.NativeLibrary` + `src/android/native` |
| Surface 生命周期 | Eden `EmulationFragment` | 后续新增 `EmulationActivity` / `SurfaceView` |
| Vulkan Android surface | Eden/Yuzu `vulkan_surface.cpp` | 已有 `src/yuzu_video_core/vulkan_common/vulkan_surface.cpp` |
| 自定义 Adreno 驱动 | Eden/Citron driver manager | 先保留 C++ adrenotools 链接，后续补 UI/文件解压 |
| SAF 文件访问 | Eden DocumentProvider/FileUtil | `yuzu_common/fs/fs_android.cpp` + Android UI |
| 输入设备 | Eden input model + Skyline/Strato gamepad | `org.nxemu.input.NxemuInputDevice` |
| ROM/Addon/Firmware 管理 | Eden/Citron installers | 复用 NXEmu `user` 目录结构，Android 映射到 app data |

## 阶段计划

### Phase 0：能打开 Android 工程

- [x] 建 `src/android` Gradle app。
- [x] 接根 CMake。
- [x] 建 `nxemu-android` JNI 桥。
- [x] 安装/配置本机 Android 构建环境：
  - JDK 17+
  - Android SDK
  - NDK `28.2.13676358`
  - CMake `3.31.6`
  - Gradle 或 Gradle Wrapper
- [x] 安装 `glslangValidator`（Vulkan SDK）。
- [x] `:app:assembleDebug` 跑通并生成 debug APK。
- [x] APK 启动时自动复制并检测内置 `hbmenu.nro`。

### 当前 Android PoC 临时补丁

- Android arm64 先走 Dynarmic，临时禁用未完整导入的 NCE 路径：
  - `src/nxemu-cpu/cpu_manager.cpp`
  - `src/nxemu-cpu/patch/patch_collection.*`
  - `src/nxemu-loader/core/loader/nro.cpp`
  - `src/nxemu-os/core/hle/kernel/k_process.cpp`
- 补 Android 构建缺口：
  - 根 CMake 调整依赖顺序，让 `yuzu_common` / `video_core` 先于 `nxemu-*` 模块可见。
  - `yuzu_common` 增加 `arm64/native_clock.cpp`。
  - `nxemu-cpu` 增加 `patch/patch_collection.cpp`。
  - Android FFmpeg 静态库链接顺序调整为 `avfilter -> avformat -> avcodec -> swscale -> swresample -> avutil`。

### Phase 1：Native 装载和模块路径

- [ ] Java 侧按顺序 `System.loadLibrary` 或由 `nxemu-android` `dlopen`：
  - `nxemu-loader`
  - `nxemu-cpu`
  - `nxemu-video`
  - `nxemu-os`
- [ ] 将模块搜索路径从 Windows `D:\NXEmu\modules` 抽象成 Android `applicationInfo.nativeLibraryDir`。
- [ ] 建 Android app internal user dir：
  - `filesDir/user`
  - `filesDir/user/keys`
  - `filesDir/user/nand`
  - `filesDir/user/addons`
  - `filesDir/user/log`

### Phase 2：Surface 和最小启动

- [ ] 新增 `EmulationActivity` + `SurfaceView`。
- [ ] JNI API：
  - `setSurface(surface: Surface?)`
  - `bootGame(uri/path)`
  - `pause/resume/stop`
- [ ] 将 `ANativeWindow` 传入 NxEmu video render window。
- [ ] 用 homebrew/sample NRO 或小 ROM 测试 loader path。

### Phase 3：文件系统/输入/设置

- [ ] SAF 选择 ROM 目录和单文件。
- [ ] keys/firmware 导入。
- [ ] addon/update/DLC 安装到 `user/addons/<title_id>`。
- [ ] 实现 `org.nxemu.input.NxemuInputDevice`，匹配 C++ Java bridge。
- [ ] 迁移最小设置页：语言、区域、CPU backend、Vulkan、resolution、driver。

### Phase 4：可用 Alpha

- [ ] Shader cache 路径和清理。
- [ ] Adreno driver manager。
- [ ] 崩溃日志导出。
- [ ] 性能 overlay / FPS。
- [ ] 游戏列表 metadata 扫描。

## 当前阻塞

当前 Windows 机器未配置 Android 构建环境：

- `gradle` 不在 PATH。
- `JAVA_HOME` 是 JDK 8：`C:\Program Files\Eclipse Adoptium\jdk-8.0.402.6-hotspot\`
- `ANDROID_HOME` / `ANDROID_SDK_ROOT` 为空。

因此当前还不能实际 `assembleDebug`。项目文件已准备好，下一步先补构建环境。
