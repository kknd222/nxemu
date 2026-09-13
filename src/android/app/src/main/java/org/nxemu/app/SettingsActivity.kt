package org.nxemu.app

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import android.view.LayoutInflater
import android.view.ViewGroup
import org.nxemu.app.databinding.ActivitySettingsBinding
import org.nxemu.app.databinding.ItemPropertyCardBinding
import org.nxemu.app.databinding.ItemPropertyHeaderBinding

class SettingsActivity : Activity() {
    private lateinit var binding: ActivitySettingsBinding
    private lateinit var adapter: SettingsAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        adapter = SettingsAdapter()
        runCatching { GpuDriverHelper.initialize(this) }
        binding.settingsList.layoutManager = GridLayoutManager(this, 2).apply {
            spanSizeLookup = object : GridLayoutManager.SpanSizeLookup() {
                override fun getSpanSize(position: Int): Int =
                    if (adapter.getItemViewType(position) == 0) spanCount else 1
            }
        }
        binding.settingsList.adapter = adapter
        binding.buttonBack.setOnClickListener { finish() }
        binding.buttonCopy.setOnClickListener { copySettingsSummary() }
        refreshUi()
    }

    private fun refreshUi() {
        binding.title.text = "Settings"
        binding.subtitle.text = "NxEmu global settings shell · 全局配置会作为每游戏默认值"
        val items = mutableListOf<Item>()
        fun header(t: String, s: String) { items += Item.Header(t, s) }
        fun card(t: String, d: String, detail: String, action: () -> Unit) { items += Item.Card(t, d, detail, action) }

        header("Performance / Graphics", "全局默认值；每游戏属性页可覆盖。")
        card("分辨率", "全局 Renderer resolution。", AppPreferences.resolutionLabel(AppPreferences.resolutionSetup(this))) {
            val next = when (AppPreferences.resolutionSetup(this)) { 1 -> 0; 0 -> 2; else -> 1 }
            AppPreferences.savePerformance(this, AppPreferences.frameSkip(this), next, AppPreferences.aspectRatio(this), AppPreferences.graphicsCompat(this), AppPreferences.preferNce(this)); refreshUi()
        }
        card("画面比例", "默认建议拉伸全屏，避免上下黑边。", AppPreferences.aspectLabel(AppPreferences.aspectRatio(this))) {
            val next = when (AppPreferences.aspectRatio(this)) { 4 -> 0; 0 -> 2; 2 -> 3; 3 -> 1; else -> 4 }
            AppPreferences.savePerformance(this, AppPreferences.frameSkip(this), AppPreferences.resolutionSetup(this), next, AppPreferences.graphicsCompat(this), AppPreferences.preferNce(this)); refreshUi()
        }
        card("跳帧", "默认 0；慢游戏再单独开。", AppPreferences.frameSkip(this).toString()) {
            AppPreferences.savePerformance(this, (AppPreferences.frameSkip(this) + 1) % 5, AppPreferences.resolutionSetup(this), AppPreferences.aspectRatio(this), AppPreferences.graphicsCompat(this), AppPreferences.preferNce(this)); refreshUi()
        }
        card("图形兼容模式", "更稳但可能慢；Kirby/Metal Dogs 可按每游戏覆盖。", if (AppPreferences.graphicsCompat(this)) "开" else "关") {
            AppPreferences.savePerformance(this, AppPreferences.frameSkip(this), AppPreferences.resolutionSetup(this), AppPreferences.aspectRatio(this), !AppPreferences.graphicsCompat(this), AppPreferences.preferNce(this)); refreshUi()
        }
        card("NCE", "Android arm64 原生 CPU 后端；当前仍是实验功能，稳定包默认关闭。", if (AppPreferences.preferNce(this)) "实验请求" else "关") {
            AppPreferences.savePerformance(this, AppPreferences.frameSkip(this), AppPreferences.resolutionSetup(this), AppPreferences.aspectRatio(this), AppPreferences.graphicsCompat(this), !AppPreferences.preferNce(this)); refreshUi()
        }
        card("性能 HUD", "精简/详细 HUD。", if (AppPreferences.perfHudDetailed(this)) "详细" else "精简") {
            AppPreferences.savePerfHudDetailed(this, !AppPreferences.perfHudDetailed(this)); refreshUi()
        }

        header("Drivers / Logs", "驱动、缓存、自动日志。")
        card("GPU Driver Manager", "打开独立驱动管理页。", GpuDriverHelper.currentSelectedDriverSource().substringAfterLast('/').ifBlank { "system/default" }) {
            startActivity(Intent(this, DriverManagerActivity::class.java))
        }
        card("清理图形/Shader 缓存", "换驱动、花屏、黑屏后使用。", "执行") {
            val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }.getOrElse { it.stackTraceToString() }
            Toast.makeText(this, "已清理缓存", Toast.LENGTH_LONG).show()
            copyText(result)
        }
        card("自动日志", "运行页自动写入 /sdcard/ns/logs。", if (AppPreferences.autoOutputLog(this)) "开" else "关") {
            AppPreferences.saveAutoOutputLog(this, !AppPreferences.autoOutputLog(this)); refreshUi()
        }

        header("Library / Storage", "游戏目录、封面、配置。")
        card("上次游戏", AppPreferences.lastGameName(this).ifBlank { "none" }, AppPreferences.lastGamePath(this).substringAfterLast('/').take(28)) { copyText(AppPreferences.lastGamePath(this)) }
        card("上次目录", AppPreferences.lastGameFolder(this).ifBlank { "none" }, "复制") { copyText(AppPreferences.lastGameFolder(this)) }
        card("显示原始 NSP/XCI", "默认隐藏原始容器，首页只显示 .dnsp/.dxci/.nro，避免误点到未转换 sibling。", if (AppPreferences.showRawSwitchContainers(this)) "显示" else "隐藏") {
            AppPreferences.saveShowRawSwitchContainers(this, !AppPreferences.showRawSwitchContainers(this))
            refreshUi()
        }
        card("封面目录", "支持 TitleID.png/jpg/webp。", "/sdcard/ns/covers") { copyText("/sdcard/ns/covers") }
        card("推荐配置 JSON", "GameProfileCatalog 文件路径。", "复制") { copyText(GameProfileCatalog.ensureProfileFile(this).absolutePath) }

        header("Input / Controls", "触控布局、透明度、手柄检测。")
        card("触控布局", "打开输入设置页；配置下次进入运行页生效。", AppPreferences.touchPresetLabel(AppPreferences.touchLayoutPreset(this))) {
            startActivity(Intent(this, InputSettingsActivity::class.java))
        }
        card("手柄映射", "外接手柄检测/映射入口。", "检测/诊断") {
            startActivity(Intent(this, InputSettingsActivity::class.java))
        }

        header("Diagnostics", "复制全局诊断方便反馈。")
        card("复制设置摘要", "复制 AppPreferences + GPU driver 状态。", "复制") { copySettingsSummary() }
        adapter.submit(items)
    }

    private fun copySettingsSummary() {
        copyText(buildString {
            appendLine("===== NxEmu Settings =====")
            appendLine(AppPreferences.statusText(this@SettingsActivity))
            appendLine(GpuDriverHelper.statusText())
        })
    }

    private fun copyText(text: String) {
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("nxemu settings", text))
        Toast.makeText(this, "已复制", Toast.LENGTH_SHORT).show()
    }

    private sealed class Item {
        data class Header(val title: String, val subtitle: String) : Item()
        data class Card(val title: String, val description: String, val detail: String, val action: () -> Unit) : Item()
    }

    private inner class SettingsAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
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
}
