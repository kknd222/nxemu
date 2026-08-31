package org.nxemu.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.PopupMenu
import android.widget.Toast
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.nxemu.app.databinding.ActivityDriverManagerBinding
import org.nxemu.app.databinding.ItemDriverCardBinding
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class DriverManagerActivity : Activity() {
    private lateinit var binding: ActivityDriverManagerBinding
    private lateinit var adapter: DriverAdapter
    private var currentSource: String = ""
    private var lastStatus: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityDriverManagerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setupUi()
        refreshDrivers("打开 GPU Driver Manager")
    }

    override fun onResume() {
        super.onResume()
        if (::binding.isInitialized) {
            runCatching { GpuDriverHelper.initialize(this) }
            refreshDrivers("刷新驱动列表")
        }
    }

    @Deprecated("Deprecated in Android API, still fine for this minimal shell")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        runCatching { persistReadPermission(uri, data.flags) }
        when (requestCode) {
            REQUEST_OPEN_DRIVER_ZIP -> {
                val result = runCatching { GpuDriverHelper.installDriverFromUri(uri) }
                    .getOrElse { "driverInstall=exception\n${it.stackTraceToString()}" }
                refreshDrivers("已安装所选驱动 ZIP", result)
            }
            REQUEST_OPEN_DRIVER_FOLDER -> {
                val result = runCatching { GpuDriverHelper.installFirstDriverFromFolder(uri) }
                    .getOrElse { "driverFolder=exception\n${it.stackTraceToString()}" }
                refreshDrivers("已从授权目录安装驱动", result)
            }
        }
    }

    private fun setupUi() {
        adapter = DriverAdapter(
            onSelect = { choice -> selectDriver(choice) },
            isSelected = { choice -> isChoiceSelected(choice) }
        )
        val driverColumns = if (resources.displayMetrics.widthPixels >= 2200) 3 else 2
        binding.driverList.layoutManager = GridLayoutManager(this, driverColumns)
        binding.driverList.adapter = adapter

        binding.buttonBack.setOnClickListener { finish() }
        binding.buttonInstallZip.setOnClickListener { openDriverZipPicker() }
        binding.buttonAutoInstall.setOnClickListener {
            val result = runCatching { GpuDriverHelper.installBestDefaultNsDriver() }
                .getOrElse { "driverAuto=exception\n${it.stackTraceToString()}" }
            refreshDrivers("自动安装 /sdcard/ns/qudong 推荐驱动", result)
        }
        binding.buttonMore.setOnClickListener { showMoreMenu() }
    }

    private fun showMoreMenu() {
        PopupMenu(this, binding.buttonMore).apply {
            menu.add("授权驱动目录").setOnMenuItemClickListener { openDriverFolderPicker(); true }
            menu.add("恢复系统 GPU 驱动").setOnMenuItemClickListener {
                val result = runCatching { GpuDriverHelper.installDefaultDriver() }
                    .getOrElse { "driverDefault=exception\n${it.stackTraceToString()}" }
                refreshDrivers("已恢复系统 GPU 驱动", result)
                true
            }
            menu.add("清理图形/驱动缓存").setOnMenuItemClickListener {
                val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }
                    .getOrElse { "clearGraphicsCaches=exception\n${it.stackTraceToString()}" }
                refreshDrivers("已清理图形/驱动缓存", result)
                true
            }
            menu.add("授权所有文件访问").setOnMenuItemClickListener { openAllFilesAccessSettings(); true }
            menu.add("复制驱动诊断").setOnMenuItemClickListener { copyDiagnosticsToClipboard(); true }
            show()
        }
    }

    private fun selectDriver(choice: GpuDriverHelper.DriverChoice) {
        val result = runCatching { GpuDriverHelper.useDriverSource(choice.source) }
            .getOrElse { "driverSelect=exception\nsource=${choice.source}\n${it.stackTraceToString()}" }
        refreshDrivers("已选择：${choice.label}", result)
        Toast.makeText(this, "已切换驱动：${choice.label}", Toast.LENGTH_SHORT).show()
    }

    private fun refreshDrivers(title: String, extra: String = "") {
        val init = runCatching { GpuDriverHelper.initialize(this) }
            .getOrElse { "driverInitialize=exception\n${it.stackTraceToString()}" }
        currentSource = runCatching { GpuDriverHelper.currentSelectedDriverSource() }.getOrDefault("")
        val choices = runCatching { GpuDriverHelper.listDriverChoices() }.getOrDefault(emptyList())
        adapter.submit(choices)
        binding.title.text = "GPU Driver Manager"
        binding.subtitle.text = "NxEmu Android 独立驱动页 · 推荐 RMX3700/Adreno 7xx 使用 Turnip 26.2/v306"
        binding.summary.text = buildString {
            appendLine(title)
            appendLine("当前：${if (currentSource.isBlank()) "系统默认 Vulkan" else File(currentSource).name}")
            appendLine("候选驱动：${choices.size} 个 · allFiles=${GpuDriverHelper.hasAllFilesAccess()}")
            append("提示：切换驱动后建议清理图形缓存并重新启动游戏。")
        }
        lastStatus = buildString {
            appendLine("===== NxEmu Android Driver Diagnostics =====")
            appendLine("time=${SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS Z", Locale.US).format(Date())}")
            appendLine("package=$packageName")
            appendLine("currentSource=${currentSource.ifBlank { "system/default" }}")
            appendLine("choices=${choices.size}")
            appendLine()
            appendLine("----- initialize -----")
            appendLine(init)
            if (extra.isNotBlank()) {
                appendLine()
                appendLine("----- last action -----")
                appendLine(extra)
            }
            appendLine()
            appendLine("----- status -----")
            appendLine(GpuDriverHelper.statusText())
            appendLine("===== End NxEmu Android Driver Diagnostics =====")
        }
        binding.statusView.text = lastStatus
    }

    private fun isChoiceSelected(choice: GpuDriverHelper.DriverChoice): Boolean {
        return if (choice.source.isBlank()) currentSource.isBlank() else choice.source == currentSource
    }

    private fun openDriverZipPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(intent, REQUEST_OPEN_DRIVER_ZIP)
    }

    private fun openDriverFolderPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        }
        startActivityForResult(intent, REQUEST_OPEN_DRIVER_FOLDER)
    }

    private fun openAllFilesAccessSettings() {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R || Environment.isExternalStorageManager()) {
            refreshDrivers("外置存储权限已可用")
            return
        }
        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
            data = Uri.parse("package:$packageName")
        }
        runCatching { startActivity(intent) }.onFailure {
            startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    private fun persistReadPermission(uri: Uri, flags: Int) {
        val takeFlags = flags and (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        if (takeFlags != 0) {
            runCatching { contentResolver.takePersistableUriPermission(uri, takeFlags) }
        }
    }

    private fun copyDiagnosticsToClipboard() {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("nxemu driver diagnostics", lastStatus))
        Toast.makeText(this, "驱动诊断已复制", Toast.LENGTH_SHORT).show()
    }

    private inner class DriverAdapter(
        private val onSelect: (GpuDriverHelper.DriverChoice) -> Unit,
        private val isSelected: (GpuDriverHelper.DriverChoice) -> Boolean
    ) : RecyclerView.Adapter<DriverAdapter.DriverViewHolder>() {
        private val items = mutableListOf<GpuDriverHelper.DriverChoice>()

        fun submit(next: List<GpuDriverHelper.DriverChoice>) {
            items.clear()
            items.addAll(next)
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): DriverViewHolder {
            return DriverViewHolder(ItemDriverCardBinding.inflate(LayoutInflater.from(parent.context), parent, false))
        }

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: DriverViewHolder, position: Int) {
            holder.bind(items[position])
        }

        inner class DriverViewHolder(private val item: ItemDriverCardBinding) : RecyclerView.ViewHolder(item.root) {
            fun bind(choice: GpuDriverHelper.DriverChoice) {
                val selected = isSelected(choice)
                item.cardRoot.setBackgroundResource(if (selected) R.drawable.eden_game_card_selected else R.drawable.eden_game_card)
                item.radioMark.text = if (selected) "●" else "○"
                item.driverTitle.text = choice.label
                item.driverDetail.text = choice.detail.ifBlank { choice.source.ifBlank { "系统默认 Vulkan 驱动" } }
                item.driverSource.text = choice.source.ifBlank { "system/default" }
                item.root.setOnClickListener { onSelect(choice) }
            }
        }
    }

    companion object {
        private const val REQUEST_OPEN_DRIVER_ZIP = 2101
        private const val REQUEST_OPEN_DRIVER_FOLDER = 2102
    }
}
