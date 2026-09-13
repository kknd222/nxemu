package org.nxemu.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.util.Locale
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.nxemu.app.databinding.ActivityPerGameSettingsBinding
import org.nxemu.app.databinding.ItemPropertyCardBinding
import org.nxemu.app.databinding.ItemPropertyHeaderBinding

class PerGameSettingsActivity : Activity() {
    private lateinit var binding: ActivityPerGameSettingsBinding
    private lateinit var propertyAdapter: PropertyAdapter
    private var gamePath: String = ""
    private var gameName: String = ""
    private lateinit var profile: AppPreferences.GameProfile
    private lateinit var summaryView: TextView
    private lateinit var cardsContainer: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        gamePath = intent.getStringExtra(EXTRA_GAME_PATH).orEmpty()
        gameName = intent.getStringExtra(EXTRA_GAME_NAME).orEmpty().ifBlank { deriveName(gamePath) }
        profile = AppPreferences.perGameProfile(this, gamePath)
        binding = ActivityPerGameSettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setupEdenPropertiesPage()
        refreshUi()
    }

    private fun setupEdenPropertiesPage() {
        summaryView = binding.summary
        propertyAdapter = PropertyAdapter()
        binding.propertiesList.layoutManager = GridLayoutManager(this, 2).apply {
            spanSizeLookup = object : GridLayoutManager.SpanSizeLookup() {
                override fun getSpanSize(position: Int): Int =
                    if (propertyAdapter.getItemViewType(position) == 0) spanCount else 1
            }
        }
        binding.propertiesList.adapter = propertyAdapter
        binding.title.text = gameName.ifBlank { "未命名游戏" }
        binding.path.text = gamePath
        binding.gameIcon.text = gameName.substringAfterLast('.', "NX").take(4).uppercase()
        applyCoverAndPlaytime()
        binding.buttonBack.setOnClickListener { finish() }
        binding.buttonStart.setOnClickListener { launchGame() }
        binding.buttonQuickDriver.setOnClickListener { showDriverChoiceDialog() }
        binding.buttonQuickLog.setOnClickListener { toggleAutoLogQuick() }
        binding.buttonQuickShader.setOnClickListener { clearGraphicsCacheQuick() }
        binding.buttonReset.setOnClickListener {
            AppPreferences.resetPerGameProfile(this@PerGameSettingsActivity, gamePath)
            profile = AppPreferences.perGameProfile(this@PerGameSettingsActivity, gamePath)
            refreshUi()
            Toast.makeText(this@PerGameSettingsActivity, "已重置为全局配置", Toast.LENGTH_SHORT).show()
        }
    }

    private fun createContentView(): ScrollView {
        summaryView = TextView(this).apply {
            textSize = 13.5f
            setTextColor(0xffd8e2ff.toInt())
            setPadding(0, 10, 0, 0)
        }
        cardsContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 12, 0, 0)
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(28, 22, 28, 26)
            setBackgroundColor(SHELL_BG_COLOR)

            addView(LinearLayout(this@PerGameSettingsActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(24, 18, 24, 18)
                background = roundedBg(0xff171b2a.toInt(), 28f, 0x445f7cff)
                addView(TextView(this@PerGameSettingsActivity).apply {
                    text = "Game Properties"
                    textSize = 28f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xffffffff.toInt())
                })
                addView(TextView(this@PerGameSettingsActivity).apply {
                    text = gameName.ifBlank { "未命名游戏" }
                    textSize = 17f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xffeaf0ff.toInt())
                    setPadding(0, 6, 0, 0)
                })
                addView(TextView(this@PerGameSettingsActivity).apply {
                    text = gamePath
                    textSize = 12.5f
                    setTextColor(0xffaeb8dc.toInt())
                    setPadding(0, 2, 0, 0)
                })
                addView(summaryView)
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, 12)
            })

            addView(LinearLayout(this@PerGameSettingsActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(topButton("返回") { finish() })
                addView(topButton("保存并启动", primary = true) { launchGame() })
                addView(topButton("重置本游戏配置") {
                    AppPreferences.resetPerGameProfile(this@PerGameSettingsActivity, gamePath)
                    profile = AppPreferences.perGameProfile(this@PerGameSettingsActivity, gamePath)
                    refreshUi()
                    Toast.makeText(this@PerGameSettingsActivity, "已重置为全局配置", Toast.LENGTH_SHORT).show()
                })
            })

            addView(cardsContainer)
        }

        return ScrollView(this).apply {
            setBackgroundColor(SHELL_BG_COLOR)
            addView(root)
        }
    }

    private fun refreshUi() {
        AppPreferences.savePerGameProfile(this, gamePath, profile)
        val preset = recommendedPreset()
        summaryView.text = buildString {
            appendLine(AppPreferences.profileSummary(this@PerGameSettingsActivity, gamePath))
            extractTitleId(gameName.ifBlank { gamePath })?.let { appendLine("TitleID=$it") }
            if (preset != null) appendLine("推荐：${preset.title}")
            appendLine(GameProfileCatalog.statusText(this@PerGameSettingsActivity))
            append("配置ID=${AppPreferences.perGameKey(gamePath).take(10)}")
        }
        binding.title.text = gameName.ifBlank { "未命名游戏" }
        binding.path.text = gamePath
        binding.gameIcon.text = gameName.substringAfterLast('.', "NX").take(4).uppercase()
        applyCoverAndPlaytime()
        binding.bottomHint.text = buildString {
            append("独立配置=${if (profile.enabled) "开" else "全局"}")
            append(" · NCE=${if (profile.preferNce) "开" else "关"}")
            append(" · res=${AppPreferences.resolutionLabel(profile.resolutionSetup)}")
            append(" · driver=${driverLabelForSource(profile.driverSource)}")
        }
        binding.buttonQuickLog.text = if (profile.autoOutputLog) "日志:开" else "日志:关"

        val items = mutableListOf<PropertyItem>()
        fun header(title: String, subtitle: String) { items += PropertyItem.Header(title, subtitle) }
        fun card(title: String, description: String, detail: String, action: () -> Unit) {
            items += PropertyItem.Card(title, description, detail, action)
        }

        header("Game info", "游戏属性页会先展示标题、路径、格式、大小和当前配置状态。")
        card(
            "文件信息",
            gamePath,
            buildString {
                append(gameName.substringAfterLast('.', "?").uppercase())
                append(" · ")
                append(formatBytes(runCatching { NativeLibrary.getSize(gamePath) }.getOrDefault(0L)))
                extractTitleId(gameName.ifBlank { gamePath })?.let { append(" · $it") }
            }
        ) { Toast.makeText(this, "路径已在顶部显示；后续会补封面/TitleID/版本信息", Toast.LENGTH_SHORT).show() }

        if (preset != null) {
        header("Recommended profile", "按游戏名/TitleID 匹配到的推荐配置，作为 per-game 调参入口。")
            card(preset.title, preset.description, "一键应用") {
                profile = preset.profile
                AppPreferences.savePerGameProfile(this, gamePath, profile)
                refreshUi()
                Toast.makeText(this, "已应用推荐配置：${preset.title}", Toast.LENGTH_LONG).show()
            }
        }

        header("Per-game settings", "这些值只在本游戏启动时覆盖全局设置；运行中切换建议重启游戏验证。")
        card("独立配置", if (profile.enabled) "本游戏使用独立 profile" else "使用全局 Performance 设置", "当前：${if (profile.enabled) "开启" else "关闭"}") {
            profile = profile.copy(enabled = !profile.enabled); refreshUi()
        }
        card("分辨率", "Renderer resolution。1/2X 更快，1X 画质更好。", AppPreferences.resolutionLabel(profile.resolutionSetup)) {
            val next = when (profile.resolutionSetup) { 1 -> 0; 0 -> 2; else -> 1 }
            profile = profile.copy(enabled = true, resolutionSetup = next); refreshUi()
        }
        card("画面比例", "默认拉伸全屏；如果上下黑边明显，优先用拉伸全屏。", AppPreferences.aspectLabel(profile.aspectRatio)) {
            val next = when (profile.aspectRatio) { 4 -> 0; 0 -> 2; 2 -> 3; 3 -> 1; else -> 4 }
            profile = profile.copy(enabled = true, aspectRatio = next); refreshUi()
        }
        card("跳帧", "默认 0。只有特定慢游戏才尝试 1~2，避免破坏动画/输入。", profile.frameSkip.toString()) {
            profile = profile.copy(enabled = true, frameSkip = (profile.frameSkip + 1) % 5); refreshUi()
        }
        card("图形兼容模式", "兼容模式会启用更稳的 reactive/high-accuracy 路线；Metal Dogs 自动推荐开启。", if (profile.graphicsCompat) "开" else "关") {
            profile = profile.copy(enabled = true, graphicsCompat = !profile.graphicsCompat); refreshUi()
        }
        card("NCE", "Android arm64 原生 CPU 后端；速度优先但仍属实验，稳定包默认关闭。", if (profile.preferNce) "实验请求" else "关闭") {
            profile = profile.copy(enabled = true, preferNce = !profile.preferNce); refreshUi()
        }
        card("性能 HUD", "精简只显示 FPS/温度/速度；详细显示 NCE/驱动/FB/present 诊断。", if (profile.perfHudDetailed) "详细" else "精简") {
            profile = profile.copy(enabled = true, perfHudDetailed = !profile.perfHudDetailed); refreshUi()
        }
        card("自动日志", "开启后本游戏运行时自动写入 /sdcard/ns/logs，适合闪退/黑屏复盘。", if (profile.autoOutputLog) "开" else "关") {
            profile = profile.copy(enabled = true, autoOutputLog = !profile.autoOutputLog); refreshUi()
        }

        header("GPU driver", "先保持 26.2 作为全局推荐；有单独问题的游戏再绑定独立驱动。")
        card("本游戏驱动", "per-game driver manager：可从已安装/默认目录候选中选择。", driverLabelForSource(profile.driverSource)) { showDriverChoiceDialog() }
        card("绑定当前全局驱动", "快捷把当前正在使用的驱动写入本游戏 profile。", runCatching { GpuDriverHelper.currentSelectedDriverSource() }.getOrDefault("").substringAfterLast('/').ifBlank { "系统/全局默认" }) {
            val current = runCatching { GpuDriverHelper.currentSelectedDriverSource() }.getOrDefault("")
            profile = profile.copy(enabled = true, driverSource = current)
            refreshUi()
            Toast.makeText(this, if (current.isBlank()) "当前是系统/全局默认驱动" else "已绑定当前驱动", Toast.LENGTH_SHORT).show()
        }

        header("Game management", "二级页面入口：信息、存档、Add-on、Shader、日志。")
        card("游戏信息", "显示 TitleID、路径、大小、profile 摘要。", "打开") { openManagement(GameManagementActivity.MODE_INFO) }
        card("存档管理", "Save Data shell：后续接导入/导出和 title save 定位。", "打开") { openManagement(GameManagementActivity.MODE_SAVE) }
        card("Add-on / DLC", "Updates/DLC 管理 shell：后续接扫描、安装、启用/禁用。", "打开") { openManagement(GameManagementActivity.MODE_ADDON) }
        card("Shader 缓存", "查看/清理 shader 与 pipeline cache。", "打开") { openManagement(GameManagementActivity.MODE_SHADER) }
        card("日志", "查看 /sdcard/ns/logs 最近日志，复制诊断。", "打开") { openManagement(GameManagementActivity.MODE_LOG) }

        header("Maintenance", "推荐配置、JSON、缓存和诊断工具。")
        card("推荐配置列表", "查看 JSON 中全部 profile；可把任意 profile 强制套用到当前游戏。", "${GameProfileCatalog.listPresets(this).size} 项") { showProfileListDialog() }
        card("保存当前为推荐配置", "把当前本游戏 profile 写入 JSON；以后按 TitleID/名称自动推荐。", "保存") {
            val result = GameProfileCatalog.savePresetForGame(this, gameName, gamePath, profile.copy(enabled = true))
            refreshUi(); Toast.makeText(this, result.lineSequence().firstOrNull().orEmpty(), Toast.LENGTH_LONG).show()
        }
        card("删除推荐配置", "从 JSON 中删除某个 profile；不会影响已经保存到本游戏的独立配置。", "选择删除") { showDeleteProfileDialog() }
        card("推荐配置 JSON", "配置库位于 /sdcard/ns/config/nxemu_game_profiles.json；可直接编辑、校验、保存。", "编辑") { showProfileJsonEditor() }
        card("复制推荐配置路径", "需要用文件管理器或电脑编辑时，复制这个路径。", "复制路径") { copyProfileCatalogPath() }
        card("复制推荐配置内容", "把整份 JSON 复制到剪贴板，方便发给我或备份。", "复制JSON") { copyProfileCatalogJson() }
        card("从剪贴板导入 JSON", "把你编辑好的整份 JSON 放在剪贴板，再点这里导入并校验。", "导入") { importProfileCatalogFromClipboard() }
        card("重置推荐配置模板", "恢复内置 Metal Dogs/Kirby 默认推荐 JSON；会覆盖当前配置库。", "重置") {
            AlertDialog.Builder(this)
                .setTitle("重置推荐配置 JSON？")
                .setMessage("这会覆盖 ${GameProfileCatalog.ensureProfileFile(this).absolutePath}")
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    val file = GameProfileCatalog.ensureProfileFile(this)
                    file.writeText(GameProfileCatalog.defaultJson(), Charsets.UTF_8)
                    refreshUi()
                    Toast.makeText(this, "已重置推荐配置 JSON", Toast.LENGTH_LONG).show()
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
        card("清理图形/Shader 缓存", "清理全局图形缓存；用于换驱动后复测黑块/花屏/黑屏。", "执行") {
            val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }
                .getOrElse { "clearGraphicsCaches=exception\n${it.stackTraceToString()}" }
            Toast.makeText(this, "已清理图形缓存", Toast.LENGTH_LONG).show()
            summaryView.text = result.take(1800)
        }
        card("复制配置摘要", "后续反馈问题时可先把这里的配置和游戏名发回来。", "复制") {
            val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
            clipboard.setPrimaryClip(android.content.ClipData.newPlainText("nxemu per-game profile", buildString {
                appendLine("game=$gameName")
                appendLine("path=$gamePath")
                appendLine(AppPreferences.profileSummary(this@PerGameSettingsActivity, gamePath))
                appendLine("profile=$profile")
                appendLine(GpuDriverHelper.summaryText())
            }))
            Toast.makeText(this, "已复制本游戏配置摘要", Toast.LENGTH_SHORT).show()
        }
        propertyAdapter.submit(items)
    }

    private fun openManagement(mode: String) {
        startActivity(Intent(this, GameManagementActivity::class.java).apply {
            putExtra(GameManagementActivity.EXTRA_MODE, mode)
            putExtra(GameManagementActivity.EXTRA_GAME_PATH, gamePath)
            putExtra(GameManagementActivity.EXTRA_GAME_NAME, gameName)
        })
    }

    private fun applyCoverAndPlaytime() {
        val cover = findGameCover()
        if (cover != null) {
            binding.gameCover.visibility = View.VISIBLE
            binding.gameCover.setImageURI(Uri.fromFile(cover))
            binding.gameIcon.setBackgroundColor(0x33000000)
        } else {
            binding.gameCover.visibility = View.GONE
            binding.gameCover.setImageDrawable(null)
            binding.gameIcon.setBackgroundResource(R.drawable.eden_badge)
        }
        binding.playtime.text = playtimeText()
    }

    private fun findGameCover(): File? {
        val titleId = extractTitleId(gameName.ifBlank { gamePath })
        val baseName = gameName.ifBlank { deriveName(gamePath) }.substringBeforeLast('.', gameName.ifBlank { deriveName(gamePath) })
        val candidates = mutableListOf<String>()
        if (!titleId.isNullOrBlank()) {
            candidates += titleId
            candidates += titleId.lowercase(Locale.US)
        }
        candidates += sanitizeCoverKey(baseName)
        candidates += sanitizeCoverKey(baseName.replace(Regex("""\[[^]]*]"""), "").trim())
        val roots = listOf(
            File("/sdcard/ns/covers"),
            File("/sdcard/ns/cover"),
            File("/sdcard/ns/rom/covers"),
            File("/sdcard/ns/roms/covers"),
            File(filesDir, "covers")
        ).distinctBy { it.absolutePath }
        val extensions = listOf("png", "jpg", "jpeg", "webp")
        roots.filter { it.exists() && it.canRead() }.forEach { root ->
            candidates.distinct().forEach { key ->
                extensions.forEach { ext ->
                    val direct = File(root, "$key.$ext")
                    if (direct.isFile && direct.length() > 0) return direct
                }
            }
        }
        return null
    }

    private fun sanitizeCoverKey(text: String): String = text.lowercase(Locale.US)
        .replace(Regex("""\.(dnsp|dxci|nsp|xci|nro)$"""), "")
        .replace(Regex("""[^a-z0-9]+"""), "_")
        .trim('_')

    private fun playtimeText(): String {
        val key = AppPreferences.perGameKey(gamePath).take(10)
        val marker = File(filesDir, "playtime/$key.txt")
        val seconds = marker.takeIf { it.exists() }?.readText()?.trim()?.toLongOrNull() ?: 0L
        if (seconds <= 0L) return "Playtime 0m"
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        return if (hours > 0) "Playtime ${hours}h ${minutes}m" else "Playtime ${minutes}m"
    }

    private fun toggleAutoLogQuick() {
        profile = profile.copy(enabled = true, autoOutputLog = !profile.autoOutputLog)
        refreshUi()
        Toast.makeText(
            this,
            if (profile.autoOutputLog) "本游戏自动日志已开启：/sdcard/ns/logs" else "本游戏自动日志已关闭",
            Toast.LENGTH_SHORT
        ).show()
    }

    private fun clearGraphicsCacheQuick() {
        val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }
            .getOrElse { "clearGraphicsCaches=exception\n${it.stackTraceToString()}" }
        summaryView.text = result.take(1800)
        Toast.makeText(this, "已清理图形/Shader 缓存", Toast.LENGTH_LONG).show()
    }

    private fun showDriverChoiceDialog() {
        val choices = runCatching { GpuDriverHelper.listDriverChoices() }.getOrDefault(emptyList())
        if (choices.isEmpty()) {
            Toast.makeText(this, "没有可选驱动；先在首页安装/授权驱动", Toast.LENGTH_LONG).show()
            return
        }
        val labels = choices.map { choice ->
            buildString {
                append(choice.label)
                if (choice.source == profile.driverSource) append("  ✓")
                append("\n")
                append(choice.detail)
            }
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("选择本游戏 GPU 驱动")
            .setItems(labels) { _, which ->
                profile = profile.copy(enabled = true, driverSource = choices[which].source)
                refreshUi()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showDeleteProfileDialog() {
        val presets = GameProfileCatalog.listPresets(this)
        if (presets.isEmpty()) {
            Toast.makeText(this, "没有可删除的推荐配置", Toast.LENGTH_LONG).show()
            return
        }
        val labels = presets.map { "${it.title}\n${it.id}" }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("删除推荐配置")
            .setItems(labels) { _, which ->
                val preset = presets[which]
                AlertDialog.Builder(this)
                    .setTitle("确认删除？")
                    .setMessage("${preset.title}\n${preset.id}")
                    .setPositiveButton(android.R.string.ok) { _, _ ->
                        val result = GameProfileCatalog.deletePreset(this, preset.id)
                        refreshUi()
                        Toast.makeText(this, result.lineSequence().firstOrNull().orEmpty(), Toast.LENGTH_LONG).show()
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showProfileListDialog() {
        val presets = GameProfileCatalog.listPresets(this)
        if (presets.isEmpty()) {
            Toast.makeText(this, "推荐配置 JSON 中没有 profile", Toast.LENGTH_LONG).show()
            return
        }
        val labels = presets.map { preset ->
            buildString {
                append(preset.title)
                append("\n")
                append(preset.id)
                append(" · skip=${preset.profile.frameSkip}")
                append(" · res=${AppPreferences.resolutionLabel(preset.profile.resolutionSetup)}")
                append(" · compat=${preset.profile.graphicsCompat}")
                if (preset.profile.driverSource.isNotBlank()) {
                    append(" · driver=${preset.profile.driverSource.substringAfterLast('/')}")
                }
            }
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("推荐配置列表")
            .setItems(labels) { _, which ->
                val preset = presets[which]
                profile = preset.profile.copy(enabled = true)
                AppPreferences.savePerGameProfile(this, gamePath, profile)
                refreshUi()
                Toast.makeText(this, "已套用：${preset.title}", Toast.LENGTH_LONG).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showProfileJsonEditor() {
        val file = GameProfileCatalog.ensureProfileFile(this)
        val editor = EditText(this).apply {
            setText(file.readText(Charsets.UTF_8))
            setSingleLine(false)
            minLines = 16
            maxLines = 28
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setHorizontallyScrolling(true)
            setTextColor(0xfff0f3ff.toInt())
            setHintTextColor(0xff9aa5c2.toInt())
            setPadding(18, 12, 18, 12)
            background = roundedBg(0xff151923.toInt(), 18f, 0x334b5d7a)
        }
        val scroll = ScrollView(this).apply {
            setPadding(18, 10, 18, 0)
            addView(editor)
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("编辑推荐配置 JSON")
            .setView(scroll)
            .setPositiveButton("保存", null)
            .setNeutralButton("复制路径", null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val text = editor.text.toString()
                val error = GameProfileCatalog.validateJson(text)
                if (error != null) {
                    editor.error = error
                    Toast.makeText(this, error, Toast.LENGTH_LONG).show()
                    return@setOnClickListener
                }
                file.writeText(org.json.JSONObject(text).toString(2), Charsets.UTF_8)
                refreshUi()
                Toast.makeText(this, "已保存并重新加载推荐配置", Toast.LENGTH_LONG).show()
                dialog.dismiss()
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                copyProfileCatalogPath()
            }
        }
        dialog.show()
    }

    private fun copyProfileCatalogPath() {
        val file = GameProfileCatalog.ensureProfileFile(this)
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("nxemu profile catalog", file.absolutePath))
        Toast.makeText(this, "已复制推荐配置路径：${file.absolutePath}", Toast.LENGTH_LONG).show()
    }

    private fun copyProfileCatalogJson() {
        val file = GameProfileCatalog.ensureProfileFile(this)
        val text = file.readText(Charsets.UTF_8)
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("nxemu game profiles json", text))
        Toast.makeText(this, "已复制推荐配置 JSON，共 ${text.length} 字符", Toast.LENGTH_LONG).show()
    }

    private fun importProfileCatalogFromClipboard() {
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
        val text = clipboard.primaryClip?.getItemAt(0)?.coerceToText(this)?.toString().orEmpty()
        if (text.isBlank()) {
            Toast.makeText(this, "剪贴板为空", Toast.LENGTH_LONG).show()
            return
        }
        val error = GameProfileCatalog.validateJson(text)
        if (error != null) {
            Toast.makeText(this, "导入失败：$error", Toast.LENGTH_LONG).show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle("从剪贴板导入推荐配置？")
            .setMessage("将覆盖 ${GameProfileCatalog.ensureProfileFile(this).absolutePath}\n\n长度：${text.length} 字符")
            .setPositiveButton(android.R.string.ok) { _, _ ->
                GameProfileCatalog.ensureProfileFile(this)
                    .writeText(org.json.JSONObject(text).toString(2), Charsets.UTF_8)
                refreshUi()
                Toast.makeText(this, "已导入推荐配置 JSON", Toast.LENGTH_LONG).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun driverLabelForSource(source: String): String {
        if (source.isBlank()) return "跟随全局"
        return runCatching {
            GpuDriverHelper.listDriverChoices().firstOrNull { it.source == source }?.label
        }.getOrNull() ?: source.substringAfterLast('/')
    }

    private fun recommendedPreset(): GamePreset? {
        val preset = GameProfileCatalog.findPreset(this, gameName, gamePath) ?: return null
        return GamePreset(preset.title, "${preset.description}\n来源：${preset.source}", preset.profile)
    }

    private fun extractTitleId(text: String): String? =
        Regex("""0100[0-9a-fA-F]{12}""").find(text)?.value?.uppercase()

    private fun formatBytes(size: Long): String {
        if (size <= 0L) return "未知大小"
        val units = arrayOf("B", "KB", "MB", "GB", "TB")
        var value = size.toDouble()
        var unit = 0
        while (value >= 1024.0 && unit < units.lastIndex) {
            value /= 1024.0
            unit++
        }
        return if (unit == 0) "${size}B" else String.format(java.util.Locale.US, "%.2f%s", value, units[unit])
    }

    private fun sectionHeader(title: String, subtitle: String): TextView =
        TextView(this).apply {
            text = "$title\n$subtitle"
            textSize = 14f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(0xffffffff.toInt())
            setPadding(12, 18, 12, 8)
        }

    private fun propertyCard(title: String, description: String, detail: String, action: () -> Unit): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(18, 14, 18, 14)
            background = roundedBg(0xcc11151f.toInt(), 24f, 0x224b5d7a)
            addView(TextView(this@PerGameSettingsActivity).apply {
                text = buildString {
                    appendLine(title)
                    append(description)
                }
                textSize = 14f
                setTextColor(0xffe8edff.toInt())
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(TextView(this@PerGameSettingsActivity).apply {
                text = detail
                textSize = 13.5f
                gravity = Gravity.CENTER
                setTextColor(0xffffffff.toInt())
                setPadding(16, 8, 16, 8)
                background = roundedBg(0xff252a36.toInt(), 18f, 0x334b5d7a)
            }, LinearLayout.LayoutParams(260, LinearLayout.LayoutParams.WRAP_CONTENT))
            setOnClickListener { action() }
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(8, 6, 8, 6)
            }
        }

    private fun topButton(label: String, primary: Boolean = false, action: () -> Unit): Button =
        Button(this).apply {
            isAllCaps = false
            text = label
            textSize = if (primary) 16.5f else 14f
            setTextColor(0xffffffff.toInt())
            background = roundedBg(if (primary) 0xff5b5ff0.toInt() else 0xff252a36.toInt(), 18f, 0x334b5d7a)
            setOnClickListener { action() }
            layoutParams = LinearLayout.LayoutParams(0, 64, 1f).apply {
                setMargins(8, 4, 8, 8)
            }
        }

    private fun launchGame() {
        AppPreferences.saveLastGame(this, gamePath, gameName)
        startActivity(Intent(this, EmulationActivity::class.java).apply {
            putExtra(EmulationActivity.EXTRA_GAME_PATH, gamePath)
            putExtra(EmulationActivity.EXTRA_GAME_NAME, gameName)
            if (profile.enabled) {
                putExtra(EmulationActivity.EXTRA_FRAME_SKIP, profile.frameSkip)
                putExtra(EmulationActivity.EXTRA_RESOLUTION_SETUP, profile.resolutionSetup)
                putExtra(EmulationActivity.EXTRA_ASPECT_RATIO, profile.aspectRatio)
                putExtra(EmulationActivity.EXTRA_GRAPHICS_COMPAT, profile.graphicsCompat)
                putExtra(EmulationActivity.EXTRA_PREFER_NCE, profile.preferNce)
                putExtra(EmulationActivity.EXTRA_PERF_HUD_DETAILED, profile.perfHudDetailed)
                putExtra(EmulationActivity.EXTRA_AUTO_OUTPUT_LOG, profile.autoOutputLog)
                if (profile.driverSource.isNotBlank()) {
                    putExtra(EmulationActivity.EXTRA_GPU_DRIVER_SOURCE, profile.driverSource)
                }
            } else {
                putExtra(EmulationActivity.EXTRA_AUTO_OUTPUT_LOG, AppPreferences.autoOutputLog(this@PerGameSettingsActivity))
            }
        })
    }

    private fun deriveName(path: String): String = when {
        path.startsWith("/") -> path.substringAfterLast('/')
        path.startsWith("content://") -> Uri.decode(path.substringAfterLast('/')).substringAfterLast(':')
        else -> Uri.decode(path).substringAfterLast('/').substringAfterLast("%2F")
    }.orEmpty()

    private fun roundedBg(color: Int, radius: Float, strokeColor: Int = 0): android.graphics.drawable.GradientDrawable =
        android.graphics.drawable.GradientDrawable().apply {
            setColor(color)
            cornerRadius = radius
            if (strokeColor != 0) setStroke(1, strokeColor)
        }

    private sealed class PropertyItem {
        data class Header(val title: String, val subtitle: String) : PropertyItem()
        data class Card(val title: String, val description: String, val detail: String, val action: () -> Unit) : PropertyItem()
    }

    private inner class PropertyAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        private val items = mutableListOf<PropertyItem>()
        fun submit(newItems: List<PropertyItem>) {
            items.clear()
            items.addAll(newItems)
            notifyDataSetChanged()
        }

        override fun getItemViewType(position: Int): Int = when (items[position]) {
            is PropertyItem.Header -> 0
            is PropertyItem.Card -> 1
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val inflater = LayoutInflater.from(parent.context)
            return if (viewType == 0) {
                HeaderHolder(ItemPropertyHeaderBinding.inflate(inflater, parent, false))
            } else {
                CardHolder(ItemPropertyCardBinding.inflate(inflater, parent, false))
            }
        }

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            when (val item = items[position]) {
                is PropertyItem.Header -> (holder as HeaderHolder).bind(item)
                is PropertyItem.Card -> (holder as CardHolder).bind(item)
            }
        }

        inner class HeaderHolder(private val binding: ItemPropertyHeaderBinding) : RecyclerView.ViewHolder(binding.root) {
            fun bind(item: PropertyItem.Header) {
                binding.title.text = item.title
                binding.subtitle.text = item.subtitle
            }
        }

        inner class CardHolder(private val binding: ItemPropertyCardBinding) : RecyclerView.ViewHolder(binding.root) {
            fun bind(item: PropertyItem.Card) {
                binding.title.text = item.title
                binding.description.text = item.description
                binding.detail.text = item.detail
                binding.cardRoot.setOnClickListener { item.action() }
            }
        }
    }

    companion object {
        const val EXTRA_GAME_PATH = "org.nxemu.app.EXTRA_GAME_PATH"
        const val EXTRA_GAME_NAME = "org.nxemu.app.EXTRA_GAME_NAME"
        private val SHELL_BG_COLOR = 0xff090b10.toInt()
    }

    private data class GamePreset(
        val title: String,
        val description: String,
        val profile: AppPreferences.GameProfile
    )
}
