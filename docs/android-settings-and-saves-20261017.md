# Android 设置与存档修复

## 原因与行为

- Android 的 `saveSettings` 原来只保存 UI 与 SettingsStore 已有 JSON，未调用四模块的
  `FlushSettings`。运行中的设置值虽已变化，但新建模拟模块会重读旧 JSON，分辨率因此回到 1x。
  现在保存以及模块重建/销毁前，均先同步模块设置。启动日志输出实际倍率、语言、区域和震动状态。
- 系统页加入全部语言选项，包括简体中文（15）、繁体中文（16）；保留游戏支持语言回退机制，
  不强制把不支持中文的游戏变成中文。界面未做完整汉化。
- 地区原来在 SetupOsSetting 中硬编码 USA；现在 RegionIndex 有独立持久化和回调，支持中国、台湾等地区。
- Android 震动收到零强度时取消，禁用全局震动后后续请求也不会触发。第一只物理手柄不再强制借用
  手机马达。暂停、后台和退出时取消当前震动，设备刷新前也取消遗留震动。

## 操作入口

先关闭正在运行的游戏，打开 Settings：

- Console / 系统 → 系统语言 / Language → 简体中文；地区 / Region → 中国。
- Console / 系统 → 手柄震动 / Vibration，关闭会立即取消手机和手柄马达。
- Graphics → Resolution，选择 0.5x、0.75x、1x、1.5x、2x 等后重新启动游戏。
  这是内部渲染倍率，不修改游戏自身报告的 720p/1080p 输出标识；不能只靠该标识判断倍率是否生效。
- 存档导入 / 导出：导入单游戏 ZIP、按游戏导出 ZIP、恢复自动备份。

## 存档规则

参考 Eden 的单游戏格式：ZIP 最外层为 16 位 Title ID 文件夹，里面是游戏存档文件。
账号存档导入当前实际用户；路径通过 OS GetProfile 的 UUID 和实际 NAND 路径解析，
遵守 nxemu 的旧版布局和已有 future-layout 优先规则，不硬编码用户 UUID。
NxEmu ZIP 另带小型存档类型信息，支持设备存档往返；Eden 无类型信息 ZIP 默认按账号存档导入。
不导入整个 NAND、固件、密钥和模拟器配置。导出列表来自已添加游戏目录，需先在游戏中保存一次。

导入先完整解压到暂存目录并校验路径/结构，再复制到目标盘的临时目录。已有存档先备份成 ZIP，
读完备份后才通过同目录重命名替换；失败时恢复原目录。备份位于应用数据目录 save_backups，
可从“恢复自动备份”入口选择，不依赖 Android 文件管理器访问 Android/data。
运行游戏或另一个存档任务时不允许管理存档，存档任务期间也禁止启动游戏。

## 验证

- 主机运行实际视频/OS 设置代码：0.5/0.75/1/1.5/2x、简体中文、中国区域、关闭震动均能保存并在
  清理/重建设置后恢复，改回默认值也通过。
- ZIP 测试覆盖 Eden 格式、路径越界/重复项/多 Title ID/空包拒绝、备份失败不动原存档、
  完整替换而非混合旧文件、原存档备份恢复、设备存档导出导入往返。
- Java 与 JavaScript 检查通过；上述测试也纳入 Android CI。Android 编译、签名及原生检查以 Actions 为准。
- 没有连接手机；真实 GPU 缩放、具体游戏中文显示、蓝牙手柄停止震动及真实存档兼容性需要手机验证。


## 安装标识与签名修正

功能构建 `053d324a` 已成功。最终交付使用独立包名 `org.nxemu.work`、应用名称 NxEmu Work，
Java/JNI 命名空间仍为 `org.nxemu`。旧版 `org.nxemu` 不会被覆盖，其目录和存档保留。
这是因为首个交付 APK 的临时 debug 私钥未实际进入旧缓存：专门恢复任务从原始缓存确认无该密钥，
无法为旧包签署可覆盖更新。不要以卸载旧版或清空数据作为解决办法。

本次在已成功编译并验证的 APK 上，仅修改二进制 manifest 的应用标识和标签，再用 Android 官方工具
zipalign/apksigner 对齐、签名。重新读取 ZIP 核对全部 DEX、原生库及资源字节未变，aapt2 检查最终包名。
源码中的 Gradle applicationId 和 manifest 标签也已同步修改；后续从源码构建即得到 Work 包。

Work 签名使用明确的 PKCS12 路径，不再依赖 AGP 默认 debug keystore 位置。CI 恢复指定密钥并校验证书指纹；
缓存丢失时必须报错，禁止悄悄生成新签名。支持通过仓库 Secrets 提供 `NXEMU_KEYSTORE_BASE64`、
`NXEMU_KEYSTORE_PASSWORD`、`NXEMU_KEY_ALIAS`、`NXEMU_KEY_PASSWORD`。
此次签名备份在 CI 中用接收方 RSA 公钥加密，公开产物中没有明文私钥；解密后的签名备份单独交付所有者。
私钥备份不要提交到公开仓库。缓存可能被淘汰，长期维护应由所有者将备份配置到仓库 Secrets。

### 从首版迁移旧存档（PC / ADB）

保留旧版安装。停止旧版后先完整备份，再考虑迁移：

```bash
adb shell am force-stop org.nxemu
adb pull /sdcard/Android/data/org.nxemu/files ./nxemu-original-backup
```

备份中一般位于 `nand/user/save/0000000000000000/<用户ID>/<TitleID>/`。
将单个 `<TitleID>` 文件夹打包为 ZIP，然后在 Work 版“存档导入 / 导出”中导入。
这会映射到 Work 当前用户，不需要猜测或强行覆盖用户 ID。先在 Work 游戏里验证存档能读，再决定旧版去留。
Work 与旧版数据隔离，首次使用需重新授权游戏目录、选择驱动；不会自动拿到旧版存档。
