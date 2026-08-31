package org.nxemu.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import android.view.LayoutInflater
import android.view.ViewGroup
import org.nxemu.app.databinding.ActivityGameManagementBinding
import org.nxemu.app.databinding.ItemPropertyCardBinding
import org.nxemu.app.databinding.ItemPropertyHeaderBinding
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

class GameManagementActivity : Activity() {
    private lateinit var binding: ActivityGameManagementBinding
    private lateinit var adapter: ManagementAdapter
    private var mode: String = MODE_INFO
    private var gamePath: String = ""
    private var gameName: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        mode = intent.getStringExtra(EXTRA_MODE).orEmpty().ifBlank { MODE_INFO }
        gamePath = intent.getStringExtra(EXTRA_GAME_PATH).orEmpty()
        gameName = intent.getStringExtra(EXTRA_GAME_NAME).orEmpty().ifBlank { deriveName(gamePath) }
        binding = ActivityGameManagementBinding.inflate(layoutInflater)
        setContentView(binding.root)
        runCatching { GpuDriverHelper.initialize(this) }
        adapter = ManagementAdapter()
        binding.itemsList.layoutManager = LinearLayoutManager(this)
        binding.itemsList.adapter = adapter
        binding.buttonBack.setOnClickListener { finish() }
        binding.buttonCopy.setOnClickListener { copyDiagnostics() }
        refreshUi()
    }

    private fun refreshUi() {
        binding.title.text = titleForMode(mode)
        binding.subtitle.text = gameName
        binding.path.text = gamePath
        binding.badge.text = badgeForMode(mode)
        val items = mutableListOf<Item>()
        fun header(title: String, subtitle: String) { items += Item.Header(title, subtitle) }
        fun card(title: String, desc: String, detail: String, action: () -> Unit = {}) { items += Item.Card(title, desc, detail, action) }

        when (mode) {
            MODE_SAVE -> {
            header("Save Data", "NxEmu save manager：导出/导入 /sdcard/ns/saves 下的 zip，避免快速闪退时丢状态。")
                val titleId = titleIdOrKey()
                val saveRoot = saveRootDir()
                val exportRoot = File("/sdcard/ns/saves").apply { mkdirs() }
                card("TitleID / Key", "从文件名解析 TitleID；没有 TitleID 时用 per-game key。", titleId)
                card("存档根目录", "NxEmu Android 私有目录中的 user save 根。", saveRoot.absolutePath) { copyText(saveRoot.absolutePath) }
                card("存档目录大小", "当前根目录递归大小。", formatBytes(saveRoot.sizeBytesRecursive()))
                card("导出存档 ZIP", "打包整个 save 根到 /sdcard/ns/saves/<TitleID>-时间.zip。", "执行") {
                    val result = exportSaveZip(saveRoot, exportRoot, titleId)
                    copyText(result)
                    Toast.makeText(this, result.lineSequence().firstOrNull().orEmpty(), Toast.LENGTH_LONG).show()
                    refreshUi()
                }
                card("导入最近 ZIP", "从 /sdcard/ns/saves 中选择当前 TitleID/key 最新 zip 解包到 save 根。", latestSaveZip(titleId)?.name ?: "未找到") {
                    val result = importLatestSaveZip(saveRoot, titleId)
                    copyText(result)
                    Toast.makeText(this, result.lineSequence().firstOrNull().orEmpty(), Toast.LENGTH_LONG).show()
                    refreshUi()
                }
                exportRoot.listFiles()?.filter { it.isFile && it.extension.equals("zip", true) }?.sortedByDescending { it.lastModified() }?.take(8)?.forEach { zip ->
                    card(zip.name, "${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(zip.lastModified()))} · ${zip.absolutePath}", formatBytes(zip.length())) { copyText(zip.absolutePath) }
                }
            }
            MODE_ADDON -> {
                header("Add-ons / Updates / DLC", "扫描 /sdcard/ns/addons；先生成 per-game addon manifest，后续接 native 装载链路。")
                val titleId = extractTitleId(gameName.ifBlank { gamePath }).orEmpty()
                val candidates = addonCandidates(titleId)
                val enabled = enabledAddons().toMutableSet()
                card("当前游戏", gameName, gamePath.substringAfterLast('/').take(64))
                card("TitleID", if (titleId.isBlank()) "文件名未携带 TitleID，将显示全部候选。" else "按 TitleID 优先过滤。", titleId.ifBlank { "unknown" })
                card("候选 Add-on", "/sdcard/ns/addons + /sdcard/ns/rom/addons + /sdcard/ns/roms/addons", "${candidates.size} 项")
                card("复制 Add-on manifest", "复制本游戏已启用 Add-on 列表。", "复制") { copyText(addonManifestFile().readTextIfExists()) }
                if (candidates.isEmpty()) {
                    card("暂无候选", "把 update/DLC 的 .dnsp/.dxci 放到 /sdcard/ns/addons 后回来刷新。", "empty")
                } else {
                    candidates.take(24).forEach { file ->
                        val path = file.absolutePath
                        val isEnabled = path in enabled
                        card(file.name, file.parent.orEmpty(), if (isEnabled) "已启用" else "未启用") {
                            if (path in enabled) enabled.remove(path) else enabled.add(path)
                            saveEnabledAddons(enabled)
                            Toast.makeText(this, if (path in enabled) "已启用 Add-on" else "已禁用 Add-on", Toast.LENGTH_SHORT).show()
                            refreshUi()
                        }
                    }
                }
            }
            MODE_SHADER -> {
                header("Shader / Pipeline Cache", "用于驱动对比、黑块/花屏/黑屏复测。")
                val dirs = shaderDirs()
                dirs.forEach { dir -> card(dir.name, dir.absolutePath, formatBytes(dir.sizeBytesRecursive())) }
                card("清理图形/Shader 缓存", "调用现有 GpuDriverHelper.clearGraphicsCaches()。", "执行") {
                    val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }
                        .getOrElse { "clearGraphicsCaches=exception\n${it.stackTraceToString()}" }
                    Toast.makeText(this, "已清理缓存", Toast.LENGTH_LONG).show()
                    copyText(result)
                    refreshUi()
                }
            }
            MODE_LOG -> {
                header("Logs / Diagnostics", "用于闪退、黑屏、性能慢时快速复制/定位日志目录。")
                val logRoot = File("/sdcard/ns/logs")
                card("日志目录", "自动日志开启后运行页会写入这里。", logRoot.absolutePath) { copyText(logRoot.absolutePath) }
                logRoot.listFiles()?.sortedByDescending { it.lastModified() }?.take(12)?.forEach { file ->
                    card(file.name, SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(file.lastModified())), formatBytes(file.length())) {
                        copyText(runCatching { file.readText().takeLast(12000) }.getOrDefault(file.absolutePath))
                    }
                } ?: card("暂无日志", "如果游戏快速退回菜单，先在属性页开启自动日志再复测。", "empty")
                card("复制运行诊断", "复制当前页面可见诊断和 native runtime status。", "复制") { copyDiagnostics() }
            }
            else -> {
            header("Game Info", "NxEmu info page shell。")
                card("名称", gameName, gameName.substringAfterLast('.', "?").uppercase(Locale.US))
                card("路径", gamePath, if (NativeLibrary.exists(gamePath)) "可读" else "未确认")
                card("大小", "NativeLibrary.getSize(path)", formatBytes(runCatching { NativeLibrary.getSize(gamePath) }.getOrDefault(0L)))
                extractTitleId(gameName.ifBlank { gamePath })?.let { card("TitleID", "从文件名解析", it) }
                card("Profile", AppPreferences.profileSummary(this, gamePath), "查看")
            }
        }
        adapter.submit(items)
    }

    private fun titleIdOrKey(): String = extractTitleId(gameName.ifBlank { gamePath }) ?: AppPreferences.perGameKey(gamePath).take(16)

    private fun saveRootDir(): File = File(filesDir, "user/nand/user/save")

    private fun exportSaveZip(saveRoot: File, exportRoot: File, titleId: String): String = runCatching {
        exportRoot.mkdirs()
        saveRoot.mkdirs()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val outFile = File(exportRoot, "$titleId-$stamp.zip")
        ZipOutputStream(FileOutputStream(outFile)).use { zip ->
            val files = saveRoot.walkTopDown().filter { it.isFile }.toList()
            if (files.isEmpty()) {
                zip.putNextEntry(ZipEntry("EMPTY_SAVE_ROOT.txt"))
                zip.write("NxEmu save root was empty at $stamp\n${saveRoot.absolutePath}\n".toByteArray(Charsets.UTF_8))
                zip.closeEntry()
            } else {
                files.forEach { file ->
                    val rel = file.relativeTo(saveRoot).invariantSeparatorsPath
                    zip.putNextEntry(ZipEntry(rel))
                    FileInputStream(file).use { input -> input.copyTo(zip) }
                    zip.closeEntry()
                }
            }
        }
        "saveExport=ok\npath=${outFile.absolutePath}\nsize=${outFile.length()}"
    }.getOrElse { "saveExport=exception\n${it.stackTraceToString()}" }

    private fun latestSaveZip(titleId: String): File? {
        val root = File("/sdcard/ns/saves")
        if (!root.exists() || !root.canRead()) return null
        return root.listFiles()
            ?.filter { it.isFile && it.extension.equals("zip", true) && it.name.contains(titleId, ignoreCase = true) }
            ?.maxByOrNull { it.lastModified() }
    }

    private fun importLatestSaveZip(saveRoot: File, titleId: String): String = runCatching {
        val zipFile = latestSaveZip(titleId) ?: return "saveImport=no-zip\nroot=/sdcard/ns/saves\nmatch=$titleId"
        saveRoot.mkdirs()
        var count = 0
        ZipInputStream(FileInputStream(zipFile)).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                val target = File(saveRoot, entry.name).canonicalFile
                val root = saveRoot.canonicalFile
                if (!target.path.startsWith(root.path + File.separator) && target != root) {
                    return "saveImport=blocked-zip-slip\nentry=${entry.name}"
                }
                if (entry.isDirectory) {
                    target.mkdirs()
                } else {
                    target.parentFile?.mkdirs()
                    FileOutputStream(target).use { out -> zip.copyTo(out) }
                    count++
                }
                zip.closeEntry()
            }
        }
        "saveImport=ok\nzip=${zipFile.absolutePath}\nfiles=$count\ntarget=${saveRoot.absolutePath}"
    }.getOrElse { "saveImport=exception\n${it.stackTraceToString()}" }

    private fun addonManifestFile(): File = File(filesDir, "addons/${AppPreferences.perGameKey(gamePath)}.txt").apply { parentFile?.mkdirs() }

    private fun enabledAddons(): Set<String> = addonManifestFile().readTextIfExists().lineSequence().map { it.trim() }.filter { it.isNotBlank() }.toSet()

    private fun saveEnabledAddons(paths: Set<String>) {
        addonManifestFile().writeText(paths.sorted().joinToString(separator = "\n", postfix = if (paths.isEmpty()) "" else "\n"), Charsets.UTF_8)
    }

    private fun addonCandidates(titleId: String): List<File> {
        val roots = listOf(File("/sdcard/ns/addons"), File("/sdcard/ns/rom/addons"), File("/sdcard/ns/roms/addons"))
        return roots.asSequence()
            .filter { it.exists() && it.canRead() }
            .flatMap { root -> root.walkTopDown().maxDepth(3).asSequence() }
            .filter { it.isFile && GamePathResolver.isSupportedGameName(it.name) }
            .filter { titleId.isBlank() || it.name.contains(titleId, ignoreCase = true) || it.parentFile?.name?.contains(titleId, ignoreCase = true) == true }
            .distinctBy { it.absolutePath }
            .sortedBy { it.name.lowercase(Locale.US) }
            .toList()
    }

    private fun File.readTextIfExists(): String = runCatching { if (exists()) readText(Charsets.UTF_8) else "" }.getOrDefault("")

    private fun copyDiagnostics() {
        copyText(buildString {
            appendLine("===== NxEmu Game Management =====")
            appendLine("mode=$mode")
            appendLine("game=$gameName")
            appendLine("path=$gamePath")
            appendLine("exists=${NativeLibrary.exists(gamePath)}")
            appendLine("size=${NativeLibrary.getSize(gamePath)}")
            appendLine(AppPreferences.profileSummary(this@GameManagementActivity, gamePath))
            appendLine(GpuDriverHelper.summaryText())
            appendLine("runtime=${runCatching { NativeLibrary.runtimeStatus() }.getOrDefault("unknown")}")
        })
    }

    private fun copyText(text: String) {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("nxemu management", text))
        Toast.makeText(this, "已复制", Toast.LENGTH_SHORT).show()
    }

    private fun shaderDirs(): List<File> = listOf(
        File(filesDir, "user/shader"),
        File(filesDir, "user/pipeline_cache"),
        File(filesDir, "user/vulkan_cache"),
        File(filesDir, "shader"),
        File(filesDir, "pipeline_cache"),
        File(getExternalFilesDir(null), "gpu/shader_cache"),
        File(getExternalFilesDir(null), "gpu/pipeline_cache")
    ).filterNotNull().distinctBy { it.absolutePath }

    private fun scanCount(path: String): String {
        val root = File(path)
        if (!root.exists() || !root.canRead()) return "0 项"
        return "${root.walkTopDown().maxDepth(2).count { it.isFile }} 项"
    }

    private fun File.sizeBytesRecursive(): Long = runCatching {
        if (!exists()) 0L else if (isFile) length() else walkTopDown().filter { it.isFile }.sumOf { it.length() }
    }.getOrDefault(0L)

    private fun titleForMode(mode: String): String = when (mode) {
        MODE_SAVE -> "Save Data"
        MODE_ADDON -> "Add-ons"
        MODE_SHADER -> "Shader Cache"
        MODE_LOG -> "Logs"
        else -> "Game Info"
    }

    private fun badgeForMode(mode: String): String = when (mode) {
        MODE_SAVE -> "SAVE"
        MODE_ADDON -> "DLC"
        MODE_SHADER -> "SHDR"
        MODE_LOG -> "LOG"
        else -> "INFO"
    }

    private fun extractTitleId(text: String): String? = Regex("""0100[0-9a-fA-F]{12}""").find(text)?.value?.uppercase(Locale.US)
    private fun deriveName(path: String): String = when {
        path.startsWith("/") -> path.substringAfterLast('/')
        path.startsWith("content://") -> Uri.decode(path.substringAfterLast('/')).substringAfterLast(':')
        else -> Uri.decode(path).substringAfterLast('/').substringAfterLast("%2F")
    }.orEmpty()

    private fun formatBytes(size: Long): String {
        if (size <= 0L) return "0B"
        val units = arrayOf("B", "KB", "MB", "GB", "TB")
        var value = size.toDouble(); var unit = 0
        while (value >= 1024.0 && unit < units.lastIndex) { value /= 1024.0; unit++ }
        return if (unit == 0) "${size}B" else String.format(Locale.US, "%.2f%s", value, units[unit])
    }

    private sealed class Item {
        data class Header(val title: String, val subtitle: String) : Item()
        data class Card(val title: String, val description: String, val detail: String, val action: () -> Unit) : Item()
    }

    private inner class ManagementAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        private val items = mutableListOf<Item>()
        fun submit(next: List<Item>) { items.clear(); items.addAll(next); notifyDataSetChanged() }
        override fun getItemViewType(position: Int): Int = if (items[position] is Item.Header) 0 else 1
        override fun getItemCount(): Int = items.size
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val inflater = LayoutInflater.from(parent.context)
            return if (viewType == 0) HeaderHolder(ItemPropertyHeaderBinding.inflate(inflater, parent, false))
            else CardHolder(ItemPropertyCardBinding.inflate(inflater, parent, false))
        }
        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            when (val item = items[position]) {
                is Item.Header -> (holder as HeaderHolder).bind(item)
                is Item.Card -> (holder as CardHolder).bind(item)
            }
        }
        inner class HeaderHolder(private val b: ItemPropertyHeaderBinding) : RecyclerView.ViewHolder(b.root) {
            fun bind(item: Item.Header) { b.title.text = item.title; b.subtitle.text = item.subtitle }
        }
        inner class CardHolder(private val b: ItemPropertyCardBinding) : RecyclerView.ViewHolder(b.root) {
            fun bind(item: Item.Card) { b.title.text = item.title; b.description.text = item.description; b.detail.text = item.detail; b.cardRoot.setOnClickListener { item.action() } }
        }
    }

    companion object {
        const val EXTRA_MODE = "org.nxemu.app.EXTRA_MODE"
        const val EXTRA_GAME_PATH = "org.nxemu.app.EXTRA_GAME_PATH"
        const val EXTRA_GAME_NAME = "org.nxemu.app.EXTRA_GAME_NAME"
        const val MODE_INFO = "info"
        const val MODE_SAVE = "save"
        const val MODE_ADDON = "addon"
        const val MODE_SHADER = "shader"
        const val MODE_LOG = "log"
    }
}
