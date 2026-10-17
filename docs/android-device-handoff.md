# Android 真机调试交接

## 代码基线

仓库： https://github.com/kknd222/nxemu
工作分支：`android/upstream-20261017`。
官方快照 `upstream/android-20261017` 和旧定制 `android-nxemu-port-20260901` 均保留。
迁移取舍见 `android-migration-20261017.md`。不要把旧分支整树覆盖到官方基线。

包名 `org.nxemu`，ARM64，最低 Android 9。Release 优化构建，未提供正式签名时
使用 CI 缓存的 debug key；这不是官方签名。若签名不匹配，不自动卸载已有应用，
先备份存档再决定处理方式。绝不能在未备份时执行 `adb uninstall` 或 `pm clear`。

## 已交付构建（2026-10-17）

- 源码提交：`a20f1d37a596c4d56d249a15c9ea21c007651c4c`。
- Actions： https://github.com/kknd222/nxemu/actions/runs/39015174487 ，成功。
- APK：`NxEmu-upstream-arm64.apk`，18,784,397 字节。
- SHA-256：`bd915a220694ea0ff0619e19037127a4af5258ac4f046491bdaee4dff28485e4`。
- 签名验证：APK v2 通过；当前为 CI debug key 签署的 Release 构建。
- 签名证书 SHA-256：`c1a30c4ca88ec42b42c31a43217ae3b64800da0b3893e2814bab93da79bff72a`。
- 所有 11 个原生库均为 ARM64；四个模拟模块和四个驱动 hook 已包含。
- 本地核对全部 25 个 `NativeLibrary` JNI 方法均有导出，四模块通用导出完整，
  原生动态依赖均能在 APK 内或 Android 系统库中找到对应名称。
- 模块诊断主机故障注入与语法检查通过；未连接手机，尚未验证真机运行和性能。

## PC Codex 开始步骤

1. 获取工作分支最新代码，读取迁移文档与本文件，并记录实际 commit。
2. 用 `adb devices -l` 确认手机；多设备时始终加 `adb -s SERIAL`。
3. `adb install -r PATH_TO_APK`，安装失败时保留原应用和数据。
4. 用 `adb shell monkey -p org.nxemu -c android.intent.category.LAUNCHER 1` 启动。
5. 开始捕获 `adb logcat -v threadtime > nxemu-logcat.txt`，手动复现问题后停止捕获。
   首先在日志中查找 `SystemModules setup failed`、`dlerror`、`Initialize=failed`、
   `Fatal signal`、`AndroidRuntime`。模块失败尚未进入游戏，不能解释为 NCE 性能问题。
6. 记录 ROM Title ID、基础版/更新版与 Build ID、手机型号/Android/GPU、驱动完整版本、
   CPU 后端、分辨率、是否外接手柄，以及复现步骤；不要上传游戏、密钥或固件内容。

## 验证顺序

- 首页、目录授权、重启后 ROM 列表、元数据读取。
- 系统驱动 / 已知兼容自定义驱动分别冷启动，确认四模块和驱动实际加载。
- 一款已知可运行游戏：出画面、声音、触控/手柄、暂停恢复、切后台、停止后第二次启动。
- 同一手机、ROM/更新、驱动、后端、分辨率和固定场景对比旧版；至少分别记录加载耗时、
  稳态帧率、卡顿和温度。显示 present 次数不能直接当游戏逻辑 FPS。
- 确认问题后只迁移仍缺少的旧修复。优先全局正确性和能力检测，不默认引入游戏配置表。

CI 成功只能证明打包、签名和原生载荷检查通过。真机启动、兼容性及性能需要上述验证。
