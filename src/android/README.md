# NxEmu Android PoC

本目录是 NxEmu Android 端口的第一阶段骨架，参考 `D:\project\switch-emulator-refs\eden\src\android` / `citron` / `suyu-mirror` 的工程形态，但先保持最小化：

- Gradle Android app：`src/android/app`
- 根 CMake 复用：`../../CMakeLists.txt`
- Android-only JNI 桥接库：`src/android/native`
- 当前 CMake 目标：
  - `nxemu-android`
  - `nxemu-loader`
  - `nxemu-cpu`
  - `nxemu-video`
  - `nxemu-os`

当前 PoC 目标是先证明：

1. Android Gradle 工程能打开；
2. NDK/CMake 能识别 NxEmu 根工程；
3. `libnxemu-android.so` 能加载，并保存 JavaVM；
4. 后续再接入 Surface、SAF 文件访问、输入、模块加载和游戏启动。

当前状态（2026-08-12）：

- 已复用本机 Android 工具链：`D:\project\qwenasr\mobile\android-webview\.tools`
- 已安装 Vulkan SDK `C:\VulkanSDK\1.4.357.0`，提供 `glslangValidator`
- `:app:assembleDebug` 已通过，产物：
  - `src/android/app/build/outputs/apk/debug/app-debug.apk`
- APK 已内置合法 homebrew 测试样本：
  - `assets/hbmenu.nro`
  - 首次启动自动复制到 `filesDir/test_roms/hbmenu.nro`
  - 当前会自动做 native NRO magic 检测；运行页会创建 `SurfaceView` 并把 `ANativeWindow` 传给 native。
- 主界面已支持文件选择和格式探测：
  - NxEmu 重点扩展名：`.dxci` / `.dnsp` / `.nro`
  - 兼容识别常见容器 magic：XCI / NSP(PFS0) / NCA / NRO
- `bootGame()` 已开始尝试走 `AppInit -> SystemModules::Setup -> Systemloader::LoadRom`；渲染/进程生命周期仍属于 PoC 阶段。
- 为先跑通 Android arm64 PoC，临时禁用了未完整接入的 NCE 代码路径；后续应参考 Eden/Citron 的 `core/arm/nce` 正式移植。

构建入口：

```powershell
cd D:\project\_nxemu_src\src\android
powershell -ExecutionPolicy Bypass -File .\build-portable.ps1 -Task ':app:assembleDebug'
```

如果使用 Android Studio，直接打开：

```text
D:\project\_nxemu_src\src\android
```

下一步建议按顺序补：

1. 从 Eden/Citron 移植 `NativeLibrary` 的 surface/game boot JNI API 形态；
2. 把 NXEmu 模块加载路径改为 Android app internal/nativeLibraryDir；
3. 接 `SurfaceView` -> `ANativeWindow` -> `WindowSystemType::Android`；
4. 接 SAF/DocumentProvider，把 ROM、keys、firmware、addons 映射到 NXEmu `AppDirectory`；
5. 接 `org.nxemu.input.NxemuInputDevice`，匹配现有 C++ `java_bridge.cpp` 期待的类名。
