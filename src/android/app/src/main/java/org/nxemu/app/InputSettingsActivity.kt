package org.nxemu.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.os.Bundle
import android.view.LayoutInflater
import android.view.ViewGroup
import android.view.InputDevice
import android.widget.Toast
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.nxemu.app.databinding.ActivityInputSettingsBinding
import org.nxemu.app.databinding.ItemPropertyCardBinding
import org.nxemu.app.databinding.ItemPropertyHeaderBinding

class InputSettingsActivity : Activity() {
    private lateinit var binding: ActivityInputSettingsBinding
    private lateinit var adapter: InputSettingsAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityInputSettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        adapter = InputSettingsAdapter()
        binding.inputList.layoutManager = GridLayoutManager(this, 2).apply {
            spanSizeLookup = object : GridLayoutManager.SpanSizeLookup() {
                override fun getSpanSize(position: Int): Int =
                    if (adapter.getItemViewType(position) == 0) spanCount else 1
            }
        }
        binding.inputList.adapter = adapter
        binding.buttonBack.setOnClickListener { finish() }
        binding.buttonCopy.setOnClickListener { copyInputDiagnostics() }
        refreshUi()
    }

    override fun onResume() {
        super.onResume()
        if (::adapter.isInitialized) refreshUi()
    }

    private fun refreshUi() {
        binding.title.text = "Input / Controls"
        binding.subtitle.text = "NxEmu control layout shell · 当前配置下次进入运行页生效"
        binding.summary.text = buildSummary()

        val items = mutableListOf<Item>()
        fun header(title: String, subtitle: String) { items += Item.Header(title, subtitle) }
        fun card(title: String, description: String, detail: String, action: () -> Unit) { items += Item.Card(title, description, detail, action) }

        header("Touch Overlay", "触控布局先提供可持久化全局预设，后续再做拖拽编辑器。")
        card("布局预设", "标准布局：左摇杆+D-pad，右摇杆+ABXY，肩键在顶部。", AppPreferences.touchPresetLabel(AppPreferences.touchLayoutPreset(this))) {
            AppPreferences.saveTouchLayoutPreset(this, (AppPreferences.touchLayoutPreset(this) + 1) % 3)
            refreshUi()
        }
        card("按键透明度", "降低透明度可减少遮挡，提高透明度方便看清按键。", "${AppPreferences.touchOpacity(this)}%") {
            val next = when (AppPreferences.touchOpacity(this)) {
                in 30..49 -> 60
                in 50..69 -> 75
                in 70..84 -> 90
                else -> 40
            }
            AppPreferences.saveTouchOpacity(this, next)
            refreshUi()
        }
        card("启动时显示触控", "外接手柄时可关闭；左上角菜单/暂停菜单仍可再显示。", if (AppPreferences.touchInitialVisible(this)) "显示" else "隐藏") {
            AppPreferences.saveTouchInitialVisible(this, !AppPreferences.touchInitialVisible(this))
            refreshUi()
        }
        card("自动淡出", "预留：一段时间无触摸后隐藏触控，触摸菜单再恢复。", if (AppPreferences.touchAutoHide(this)) "开" else "关") {
            AppPreferences.saveTouchAutoHide(this, !AppPreferences.touchAutoHide(this))
            refreshUi()
        }

        header("Gamepad", "Android InputDevice 检测；Switch 语义映射仍走 nxemu input bridge。")
        val gamepads = connectedGamepads()
        if (gamepads.isEmpty()) {
            card("未检测到外接手柄", "蓝牙/USB 手柄连接后回到本页刷新。", "none") { refreshUi() }
        } else {
            gamepads.forEach { device ->
                card(device.name, "id=${device.id} · sources=0x${device.sources.toString(16)}", "detected") { copyText(deviceDiagnostic(device)) }
            }
        }
        card("刷新手柄列表", "重新读取 Android InputDevice。", "刷新") { refreshUi() }

        header("Diagnostics", "复制输入/触控配置，方便后续调布局和手柄。")
        card("复制输入诊断", "包含触控预设、透明度、设备列表。", "复制") { copyInputDiagnostics() }
        adapter.submit(items)
    }

    private fun connectedGamepads(): List<InputDevice> = InputDevice.getDeviceIds()
        .toList()
        .mapNotNull { id -> runCatching { InputDevice.getDevice(id) }.getOrNull() }
        .filter { device: InputDevice ->
            val sources = device.sources
            (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
                (sources and InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD
        }
        .sortedBy { device: InputDevice -> device.name.lowercase() }

    private fun buildSummary(): String = buildString {
        append("触控=${AppPreferences.touchPresetLabel(AppPreferences.touchLayoutPreset(this@InputSettingsActivity))} · ")
        append("透明度=${AppPreferences.touchOpacity(this@InputSettingsActivity)}% · ")
        append("启动=${if (AppPreferences.touchInitialVisible(this@InputSettingsActivity)) "显示" else "隐藏"} · ")
        append("手柄=${connectedGamepads().size} 个")
    }

    private fun deviceDiagnostic(device: InputDevice): String = buildString {
        appendLine("device=${device.name}")
        appendLine("id=${device.id}")
        appendLine("descriptor=${device.descriptor}")
        appendLine("sources=0x${device.sources.toString(16)}")
        appendLine("vendor=${device.vendorId} product=${device.productId}")
    }

    private fun copyInputDiagnostics() {
        copyText(buildString {
            appendLine("===== NxEmu Input Settings =====")
            appendLine(AppPreferences.statusText(this@InputSettingsActivity))
            appendLine("----- Input devices -----")
            connectedGamepads().forEach { appendLine(deviceDiagnostic(it)) }
        })
    }

    private fun copyText(text: String) {
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("nxemu input", text))
        Toast.makeText(this, "已复制", Toast.LENGTH_SHORT).show()
    }

    private sealed class Item {
        data class Header(val title: String, val subtitle: String) : Item()
        data class Card(val title: String, val description: String, val detail: String, val action: () -> Unit) : Item()
    }

    private inner class InputSettingsAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
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
