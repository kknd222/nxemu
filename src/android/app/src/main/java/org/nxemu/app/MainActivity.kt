package org.nxemu.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import android.text.Editable
import android.text.TextWatcher
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.LayoutInflater
import android.view.ViewGroup
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import android.widget.PopupMenu
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.RecyclerView
import org.nxemu.app.databinding.ActivityMainBinding
import org.nxemu.app.databinding.ItemGameCardBinding
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

class MainActivity : Activity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var gameAdapter: EdenGameAdapter
    private lateinit var statusView: TextView
    private lateinit var selectedGameView: TextView
    private lateinit var gameLibraryContainer: LinearLayout
    private lateinit var gameFilterInput: EditText
    private lateinit var shellSummaryView: TextView
    private lateinit var diagnosticsToggleButton: View
    private lateinit var sampleNro: File
    private var selectedGamePath: String? = null
    private var selectedGameName: String = ""
    private var selectedFolderPath: String = ""
    private var scannedGames: List<GameEntry> = emptyList()
    private var gameFilterText: String = ""
    private var diagnosticsExpanded: Boolean = false
    private var homeViewMode: Int = 0
    private var homeSortMode: Int = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE

        sampleNro = File(filesDir, "test_roms/hbmenu.nro")
        selectedGamePath = AppPreferences.lastGamePath(this).ifBlank { null }
        selectedGameName = AppPreferences.lastGameName(this)
        normalizeSelectedGameName()
        selectedFolderPath = AppPreferences.lastGameFolder(this)
        homeViewMode = AppPreferences.homeViewMode(this)
        homeSortMode = AppPreferences.homeSortMode(this)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setupEdenHome()
        val autoScanTitle = autoScanGameLibrary()
        val startupGame = selectedGamePath ?: ensureBundledSample().absolutePath
        val startupTitle = autoScanTitle ?: if (selectedGamePath.isNullOrBlank()) "启动检测：内置 hbmenu.nro" else "已恢复上次游戏"
        refreshStatus(game = startupGame, title = startupTitle)
        handleHomeAction(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleHomeAction(intent)
    }

    override fun onResume() {
        super.onResume()
        if (::statusView.isInitialized && ::sampleNro.isInitialized) {
            runCatching {
                // 从“所有文件访问”设置页回来后刷新权限/驱动状态。
                GpuDriverHelper.initialize(this)
            }
        }
    }

    @Deprecated("Deprecated in Android API, still fine for this minimal PoC")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) {
            return
        }

        val uri = data?.data ?: return
        when (requestCode) {
            REQUEST_OPEN_GAME -> {
                runCatching {
                    persistReadPermission(uri, data.flags)
                    selectedGamePath = importGame(uri)
                    selectedGameName = queryDisplayName(uri).ifBlank { uri.lastPathSegment.orEmpty() }
                    AppPreferences.saveLastGame(this, selectedGamePath, selectedGameName)
                    updateGameLibraryUi()
                    refreshStatus(selectedGamePath, title = "已选择 DXCI/DNSP/NRO")
                }.onFailure { error ->
                    statusView.text = "导入失败：\n${error.stackTraceToString()}"
                }
            }

            REQUEST_OPEN_GAME_FOLDER -> {
                runCatching {
                    persistReadPermission(uri, data.flags)
                    selectedFolderPath = uri.toString()
                    AppPreferences.saveLastGameFolder(this, selectedFolderPath)
                    scannedGames = scanGameFolder(uri)
                    val first = scannedGames.firstOrNull()
                    if (first != null) {
                        selectedGamePath = first.uri.toString()
                        selectedGameName = first.name
                        AppPreferences.saveLastGame(this, selectedGamePath, selectedGameName)
                    }
                    updateGameLibraryUi()
                    refreshStatus(selectedGamePath, title = "已授权外置目录，扫描到 ${scannedGames.size} 个文件")
                }.onFailure { error ->
                    statusView.text = "授权/扫描目录失败：\n${error.stackTraceToString()}"
                }
            }

            REQUEST_OPEN_DRIVER_ZIP -> {
                runCatching {
                    persistReadPermission(uri, data.flags)
                    val result = GpuDriverHelper.installDriverFromUri(uri)
                    refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已安装 GPU 驱动")
                    statusView.append("\n\nDriver install:\n$result")
                }.onFailure { error ->
                    statusView.text = "驱动安装失败：\n${error.stackTraceToString()}"
                }
            }

            REQUEST_OPEN_DRIVER_FOLDER -> {
                runCatching {
                    persistReadPermission(uri, data.flags)
                    val result = GpuDriverHelper.installFirstDriverFromFolder(uri)
                    refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已授权/安装驱动目录")
                    statusView.append("\n\nDriver folder install:\n$result")
                }.onFailure { error ->
                    statusView.text = "驱动目录授权/安装失败：\n${error.stackTraceToString()}"
                }
            }
        }
    }

    private fun setupEdenHome() {
        statusView = binding.statusView
        selectedGameView = binding.selectedGame
        shellSummaryView = binding.shellSummary
        diagnosticsToggleButton = binding.buttonMore
        gameFilterInput = binding.searchText
        normalizeEdenToolbarBounds()

        gameAdapter = EdenGameAdapter(
            onSelect = { entry -> selectGameEntry(entry, launch = false) },
            onLaunch = { entry -> selectGameEntry(entry, launch = true) },
            onProperties = { entry ->
                selectGameEntry(entry, launch = false)
                openPerGameSettings(entry.uri.toString(), entry.name)
            },
            isSelected = { entry -> selectedGamePath == entry.uri.toString() }
        )
        applyHomeViewMode()
        updateSortButton()
        binding.gameList.adapter = gameAdapter

        binding.searchText.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                gameFilterText = s?.toString().orEmpty()
                binding.buttonClearSearch.visibility = if (gameFilterText.isBlank()) View.GONE else View.VISIBLE
                updateGameLibraryUi()
            }
            override fun afterTextChanged(s: Editable?) = Unit
        })
        binding.buttonClearSearch.setOnClickListener {
            binding.searchText.setText("")
            binding.searchText.clearFocus()
        }
        binding.buttonScanDefault.setOnClickListener {
            scannedGames = dedupeGameEntries(scanDefaultNsRomFolder())
            if (scannedGames.isNotEmpty()) {
                selectedFolderPath = "/sdcard/ns/rom"
                AppPreferences.saveLastGameFolder(this@MainActivity, selectedFolderPath)
            }
            selectBestScannedGameIfNeeded(force = true)
            updateGameLibraryUi()
            refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "直接扫描 /sdcard/ns/rom：${scannedGames.size} 个文件")
        }
        binding.buttonOpenFolder.setOnClickListener { openGameFolderPicker() }
        binding.buttonSortMode.setOnClickListener { showSortModeMenu() }
        binding.buttonViewMode.setOnClickListener {
            homeViewMode = (homeViewMode + 1) % 3
            AppPreferences.saveHomeViewMode(this@MainActivity, homeViewMode)
            applyHomeViewMode()
            updateGameLibraryUi()
        }
        binding.buttonDriverManager.setOnClickListener { openDriverManager() }
        binding.buttonSettings.setOnClickListener { openSettings() }
        binding.buttonInputSettings.setOnClickListener { openInputSettings() }
        binding.buttonRefreshCovers.setOnClickListener { refreshCoversAndLibrary() }
        binding.buttonRunSelected.setOnClickListener {
            val game = selectedGamePath
            if (game.isNullOrBlank() || !NativeLibrary.exists(game)) {
                statusView.text = "还没有选择可读文件。先点“扫描ROM”、“授权目录”或在更多里选择文件。"
            } else {
                AppPreferences.saveLastGame(this@MainActivity, game, selectedGameName)
                startEmulation(game)
            }
        }
        binding.buttonGameProperties.setOnClickListener {
            val game = selectedGamePath
            if (game.isNullOrBlank()) {
                statusView.text = "还没有选择游戏。先在游戏库点一个卡片，再进入游戏属性。"
            } else {
                openPerGameSettings(game, effectiveSelectedGameName())
            }
        }
        binding.buttonMore.setOnClickListener { openHomeMenu() }
        updateGameLibraryUi()
        updateDiagnosticsVisibility()
    }

    private fun handleHomeAction(intent: Intent?) {
        val action = intent?.getStringExtra(EXTRA_HOME_ACTION).orEmpty()
        if (action.isBlank()) return
        intent?.removeExtra(EXTRA_HOME_ACTION)
        when (action) {
            HOME_ACTION_OPEN_FILE -> openGamePicker()
            HOME_ACTION_OPEN_FOLDER -> openGameFolderPicker()
            HOME_ACTION_SCAN_DEFAULT -> {
                scannedGames = dedupeGameEntries(scanDefaultNsRomFolder())
                if (scannedGames.isNotEmpty()) {
                    selectedFolderPath = "/sdcard/ns/rom"
                    AppPreferences.saveLastGameFolder(this@MainActivity, selectedFolderPath)
                }
                selectBestScannedGameIfNeeded(force = true)
                updateGameLibraryUi()
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "直接扫描 /sdcard/ns/rom：${scannedGames.size} 个文件")
            }
            HOME_ACTION_RUN_LAST -> {
                val game = AppPreferences.lastGamePath(this@MainActivity)
                if (game.isBlank() || !NativeLibrary.exists(game)) {
                    statusView.text = "没有可用的上次游戏记录。先选择/授权一次游戏。\nlastGame=$game"
                } else {
                    selectedGamePath = game
                    selectedGameName = AppPreferences.lastGameName(this@MainActivity)
                    startEmulation(game)
                }
            }
            HOME_ACTION_REFRESH_COVERS -> refreshCoversAndLibrary()
            HOME_ACTION_COPY_LOG -> copyDiagnosticsToClipboard()
            HOME_ACTION_ALL_FILES -> openAllFilesAccessSettings()
        }
    }

    private fun autoScanGameLibrary(): String? {
        val entries = mutableListOf<GameEntry>()
        entries += runCatching { scanDefaultNsRomFolder() }.getOrDefault(emptyList())

        val folder = selectedFolderPath
        if (folder.startsWith("content://", ignoreCase = true)) {
            val uri = Uri.parse(folder)
            val hasPersistedPermission = contentResolver.persistedUriPermissions.any {
                it.uri == uri && it.isReadPermission
            }
            if (hasPersistedPermission) {
                entries += runCatching { scanGameFolder(uri) }.getOrDefault(emptyList())
            }
        }

        scannedGames = dedupeGameEntries(entries)
        if (scannedGames.isEmpty()) {
            updateGameLibraryUi()
            return null
        }

        selectedFolderPath = selectedFolderPath.ifBlank { "/sdcard/ns/rom" }
        AppPreferences.saveLastGameFolder(this@MainActivity, selectedFolderPath)
        selectBestScannedGameIfNeeded(force = false)
        updateGameLibraryUi()
        return "启动自动扫描：${scannedGames.size} 个游戏/程序"
    }

    private fun selectBestScannedGameIfNeeded(force: Boolean) {
        if (scannedGames.isEmpty()) return
        val current = selectedGamePath.orEmpty()
        val currentEntry = scannedGames.firstOrNull { sameGamePath(it.uri.toString(), current) }
        val chosen = when {
            !force && currentEntry != null -> currentEntry
            else -> scannedGames.firstOrNull()
        } ?: return
        selectedGamePath = chosen.uri.toString()
        selectedGameName = chosen.name
        AppPreferences.saveLastGame(this@MainActivity, selectedGamePath, selectedGameName)
    }

    private fun sameGamePath(a: String, b: String): Boolean {
        if (a == b) return true
        val na = normalizeGamePathKey(a)
        val nb = normalizeGamePathKey(b)
        return na.isNotBlank() && na == nb
    }

    private fun normalizeEdenToolbarBounds() {
        binding.buttonMore.layoutParams = LinearLayout.LayoutParams(dp(38), dp(38)).apply {
            marginStart = dp(8)
        }
        binding.buttonMore.visibility = View.VISIBLE
        binding.buttonRefreshCovers.layoutParams = FrameLayout.LayoutParams(dp(34), dp(34), Gravity.END or Gravity.CENTER_VERTICAL)
        binding.buttonRefreshCovers.visibility = View.GONE
        binding.buttonClearSearch.layoutParams = FrameLayout.LayoutParams(dp(34), dp(34), Gravity.END or Gravity.CENTER_VERTICAL).apply {
            marginEnd = dp(40)
        }
    }

    private fun applyHomeViewMode() {
        if (!::binding.isInitialized) return
        val columns = when (homeViewMode) {
            HOME_VIEW_LIST -> 1
            HOME_VIEW_COMPACT -> 5
            else -> 4
        }
        val current = binding.gameList.layoutManager as? GridLayoutManager
        if (current == null || current.spanCount != columns) {
            binding.gameList.layoutManager = GridLayoutManager(this, columns)
        }
        if (::gameAdapter.isInitialized) {
            gameAdapter.setViewMode(homeViewMode)
        }
        binding.buttonViewMode.setImageResource(
            when (homeViewMode) {
                HOME_VIEW_LIST -> R.drawable.nx_ic_view_list
                HOME_VIEW_COMPACT -> R.drawable.nx_ic_view_grid
                else -> R.drawable.nx_ic_view_grid
            }
        )
        binding.buttonViewMode.contentDescription = when (homeViewMode) {
            HOME_VIEW_LIST -> "List"
            HOME_VIEW_COMPACT -> "Compact"
            else -> "Grid"
        }
    }

    private fun updateSortButton() {
        if (!::binding.isInitialized) return
        binding.buttonSortMode.contentDescription = sortModeLabel(homeSortMode)
    }

    private fun showSortModeMenu() {
        PopupMenu(this, binding.buttonSortMode).apply {
            val modes = listOf(
                HOME_SORT_RECOMMENDED to "推荐 / 本体优先",
                HOME_SORT_NAME_ASC to "名称 A-Z",
                HOME_SORT_NAME_DESC to "名称 Z-A",
                HOME_SORT_SIZE_DESC to "大小 大-小",
                HOME_SORT_SIZE_ASC to "大小 小-大",
                HOME_SORT_FORMAT to "格式 DNSP/DXCI 优先"
            )
            modes.forEach { (mode, label) ->
                menu.add(if (homeSortMode == mode) "✓ $label" else label).setOnMenuItemClickListener {
                    homeSortMode = mode
                    AppPreferences.saveHomeSortMode(this@MainActivity, homeSortMode)
                    updateSortButton()
                    updateGameLibraryUi()
                    true
                }
            }
            show()
        }
    }

    private fun sortModeLabel(mode: Int): String = when (mode.coerceIn(0, 5)) {
        HOME_SORT_NAME_ASC -> "A-Z"
        HOME_SORT_NAME_DESC -> "Z-A"
        HOME_SORT_SIZE_DESC -> "大-小"
        HOME_SORT_SIZE_ASC -> "小-大"
        HOME_SORT_FORMAT -> "格式"
        else -> "推荐"
    }

    private fun sortGames(entries: List<GameEntry>): List<GameEntry> = when (homeSortMode.coerceIn(0, 5)) {
        HOME_SORT_NAME_ASC -> entries.sortedBy { it.name.lowercase(Locale.US) }
        HOME_SORT_NAME_DESC -> entries.sortedByDescending { it.name.lowercase(Locale.US) }
        HOME_SORT_SIZE_DESC -> entries.sortedWith(compareByDescending<GameEntry> { it.size }.thenBy { it.name.lowercase(Locale.US) })
        HOME_SORT_SIZE_ASC -> entries.sortedWith(compareBy<GameEntry> { if (it.size <= 0L) Long.MAX_VALUE else it.size }.thenBy { it.name.lowercase(Locale.US) })
        HOME_SORT_FORMAT -> entries.sortedWith(compareByDescending<GameEntry> { formatSortScore(it.name) }.thenBy { it.name.lowercase(Locale.US) })
        else -> entries.sortedWith(compareByDescending<GameEntry> { classifyGameScore(it.name) }.thenBy { it.name.lowercase(Locale.US) })
    }

    private fun selectGameEntry(entry: GameEntry, launch: Boolean) {
        selectedGamePath = entry.uri.toString()
        selectedGameName = entry.name
        AppPreferences.saveLastGame(this@MainActivity, selectedGamePath, selectedGameName)
        updateGameLibraryUi()
        refreshStatus(selectedGamePath, title = "已从游戏库选择：${entry.name}")
        if (launch) startEmulation(entry.uri.toString())
    }

    private fun showEdenMoreMenu() {
        PopupMenu(this, binding.buttonMore).apply {
            menu.add("选择 DXCI/DNSP/NRO 文件").setOnMenuItemClickListener { openGamePicker(); true }
            menu.add("运行上次游戏").setOnMenuItemClickListener {
                val game = AppPreferences.lastGamePath(this@MainActivity)
                if (game.isBlank() || !NativeLibrary.exists(game)) {
                    statusView.text = "没有可用的上次游戏记录。先选择/授权一次游戏。\nlastGame=$game"
                } else {
                    selectedGamePath = game
                    selectedGameName = AppPreferences.lastGameName(this@MainActivity)
                    startEmulation(game)
                }
                true
            }
            menu.add("当前游戏属性/独立配置").setOnMenuItemClickListener {
                val game = selectedGamePath
                if (game.isNullOrBlank()) {
                    statusView.text = "还没有选择游戏。先在游戏库点一个卡片。"
                } else {
                    openPerGameSettings(game, effectiveSelectedGameName())
                }
                true
            }
            menu.add("全局设置").setOnMenuItemClickListener { openSettings(); true }
            menu.add("输入/触控设置").setOnMenuItemClickListener { openInputSettings(); true }
            menu.add("刷新封面缓存").setOnMenuItemClickListener { refreshCoversAndLibrary(); true }
            menu.add("GPU 驱动管理").setOnMenuItemClickListener { openDriverManager(); true }
            menu.add("自动安装 /sdcard/ns/qudong 驱动").setOnMenuItemClickListener {
                val result = runCatching { GpuDriverHelper.installBestDefaultNsDriver() }
                    .getOrElse { error -> "driverInstall=exception\n${error.stackTraceToString()}" }
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "自动安装 GPU 驱动")
                statusView.append("\n\nDriver install:\n$result")
                true
            }
            menu.add("选择 Turnip/Adreno 驱动 ZIP").setOnMenuItemClickListener { openDriverZipPicker(); true }
            menu.add("授权驱动目录").setOnMenuItemClickListener { openDriverFolderPicker(); true }
            menu.add("恢复系统 GPU 驱动").setOnMenuItemClickListener {
                val result = runCatching { GpuDriverHelper.installDefaultDriver() }
                    .getOrElse { error -> "driverDefault=exception\n${error.stackTraceToString()}" }
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "恢复系统 GPU 驱动")
                statusView.append("\n\nDriver default:\n$result")
                true
            }
            menu.add("清理图形/驱动缓存").setOnMenuItemClickListener {
                val result = runCatching { GpuDriverHelper.clearGraphicsCaches() }
                    .getOrElse { error -> "clearGraphicsCaches=exception\n${error.stackTraceToString()}" }
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已清理图形/驱动缓存；建议重新启动游戏复测")
                statusView.append("\n\nGraphics cache clear:\n$result")
                true
            }
            menu.add(if (AppPreferences.autoOutputLog(this@MainActivity)) "自动日志：关闭" else "自动日志：开启").setOnMenuItemClickListener {
                val enabled = !AppPreferences.autoOutputLog(this@MainActivity)
                AppPreferences.saveAutoOutputLog(this@MainActivity, enabled)
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "自动输出日志已${if (enabled) "开启" else "关闭"}")
                true
            }
            menu.add(if (diagnosticsExpanded) "隐藏诊断详情" else "显示诊断详情").setOnMenuItemClickListener {
                diagnosticsExpanded = !diagnosticsExpanded
                updateDiagnosticsVisibility()
                true
            }
            menu.add("复制首页日志").setOnMenuItemClickListener { copyDiagnosticsToClipboard(); true }
            menu.add("排序/过滤：${sortModeLabel(homeSortMode)}").setOnMenuItemClickListener {
                showSortModeMenu()
                true
            }
            menu.add("切换视图：Grid/List/Compact").setOnMenuItemClickListener {
                homeViewMode = (homeViewMode + 1) % 3
                AppPreferences.saveHomeViewMode(this@MainActivity, homeViewMode)
                applyHomeViewMode()
                updateGameLibraryUi()
                true
            }
            menu.add("授权所有文件访问/外置驱动").setOnMenuItemClickListener { openAllFilesAccessSettings(); true }
            show()
        }
    }

    private fun createContentView(): ScrollView {
        statusView = TextView(this).apply {
            textSize = 13f
            typeface = Typeface.MONOSPACE
            setTextColor(0xffd8e2ff.toInt())
            setPadding(22, 18, 22, 18)
            background = roundedBg(0xaa151923.toInt(), 22f, 0x334b5d7a)
        }

        val selectButton = Button(this).apply {
            text = "选择 DXCI/DNSP 游戏或 NRO"
            setOnClickListener { openGamePicker() }
        }

        val selectFolderButton = Button(this).apply {
            text = "授权外置游戏目录"
            setOnClickListener { openGameFolderPicker() }
        }

        val scanDefaultRomButton = Button(this).apply {
            text = "扫描 /sdcard/ns/rom"
            setOnClickListener {
                scannedGames = scanDefaultNsRomFolder()
                val first = scannedGames.firstOrNull()
                if (first != null) {
                    selectedGamePath = first.uri.toString()
                    selectedGameName = first.name
                    selectedFolderPath = "/sdcard/ns/rom"
                    AppPreferences.saveLastGame(this@MainActivity, selectedGamePath, selectedGameName)
                }
                updateGameLibraryUi()
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "直接扫描 /sdcard/ns/rom：${scannedGames.size} 个文件")
            }
        }

        val bundledButton = Button(this).apply {
            text = "检测内置测试 hbmenu.nro"
            setOnClickListener {
                refreshStatus(ensureBundledSample(forceCopy = true).absolutePath, title = "内置 hbmenu.nro")
            }
        }

        val runBundledButton = Button(this).apply {
            text = "运行内置测试 hbmenu.nro"
            setOnClickListener { startEmulation(ensureBundledSample().absolutePath) }
        }

        val runSelectedButton = Button(this).apply {
            text = "运行已选择游戏/程序"
            setOnClickListener {
                val game = selectedGamePath
                if (game.isNullOrBlank() || !NativeLibrary.exists(game)) {
                    statusView.text = "还没有选择可读文件。先点“选择 DXCI/DNSP 游戏或 NRO”。"
                } else {
                    AppPreferences.saveLastGame(this@MainActivity, game, selectedGameName)
                    startEmulation(game)
                }
            }
        }

        val runLastButton = Button(this).apply {
            text = "运行上次游戏"
            setOnClickListener {
                val game = AppPreferences.lastGamePath(this@MainActivity)
                if (game.isBlank() || !NativeLibrary.exists(game)) {
                    statusView.text = "没有可用的上次游戏记录。先选择/授权一次游戏。\nlastGame=$game"
                } else {
                    selectedGamePath = game
                    selectedGameName = AppPreferences.lastGameName(this@MainActivity)
                    startEmulation(game)
                }
            }
        }

        val autoDriverButton = Button(this).apply {
            text = "自动安装 /sdcard/ns/qudong 驱动"
            setOnClickListener {
                val result = runCatching {
                    GpuDriverHelper.installBestDefaultNsDriver()
                }.getOrElse { error -> "driverInstall=exception\n${error.stackTraceToString()}" }
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "自动安装 GPU 驱动")
                statusView.append("\n\nDriver install:\n$result")
            }
        }

        val allFilesAccessButton = Button(this).apply {
            text = "授权所有文件访问/外置驱动"
            setOnClickListener { openAllFilesAccessSettings() }
        }

        val selectDriverZipButton = Button(this).apply {
            text = "选择 Turnip/Adreno 驱动 ZIP"
            setOnClickListener { openDriverZipPicker() }
        }

        val selectDriverFolderButton = Button(this).apply {
            text = "授权驱动目录"
            setOnClickListener { openDriverFolderPicker() }
        }

        val systemDriverButton = Button(this).apply {
            text = "恢复系统 GPU 驱动"
            setOnClickListener {
                val result = runCatching {
                    GpuDriverHelper.installDefaultDriver()
                }.getOrElse { error -> "driverDefault=exception\n${error.stackTraceToString()}" }
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "恢复系统 GPU 驱动")
                statusView.append("\n\nDriver default:\n$result")
            }
        }

        val gamePropertiesButton = Button(this).apply {
            text = "游戏属性 / 独立配置"
            setOnClickListener {
                val game = selectedGamePath
                if (game.isNullOrBlank()) {
                    statusView.text = "还没有选择游戏。先在游戏库点一个卡片，再进入游戏属性。"
                } else {
                    openPerGameSettings(game, effectiveSelectedGameName())
                }
            }
        }

        val clearGraphicsCacheButton = Button(this).apply {
            text = "清理图形/驱动缓存"
            setOnClickListener {
                val result = runCatching {
                    GpuDriverHelper.clearGraphicsCaches()
                }.getOrElse { error -> "clearGraphicsCaches=exception\n${error.stackTraceToString()}" }
                refreshStatus(
                    selectedGamePath ?: sampleNro.absolutePath,
                    title = "已清理图形/驱动缓存；建议重新启动游戏复测黑方块/花屏"
                )
                statusView.append("\n\nGraphics cache clear:\n$result")
                Toast.makeText(
                    this@MainActivity,
                    "已清理图形缓存。请重新启动卡比复测黑色方块。",
                    Toast.LENGTH_LONG
                ).show()
            }
        }

        val autoOutputLogButton = Button(this).apply {
            fun refreshText() {
                text = if (AppPreferences.autoOutputLog(this@MainActivity)) {
                    "自动输出日志：开 (/sdcard/ns/logs)"
                } else {
                    "自动输出日志：关"
                }
            }
            refreshText()
            setOnClickListener {
                val enabled = !AppPreferences.autoOutputLog(this@MainActivity)
                AppPreferences.saveAutoOutputLog(this@MainActivity, enabled)
                refreshText()
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "自动输出日志已${if (enabled) "开启" else "关闭"}")
                Toast.makeText(
                    this@MainActivity,
                    if (enabled) "运行时会自动写入 /sdcard/ns/logs" else "已关闭运行时自动日志",
                    Toast.LENGTH_LONG
                ).show()
            }
        }

        val copyLogButton = Button(this).apply {
            text = "复制首页日志"
            setOnClickListener { copyDiagnosticsToClipboard() }
        }
        diagnosticsToggleButton = Button(this).apply {
            isAllCaps = false
            text = "显示诊断详情"
            setOnClickListener {
                diagnosticsExpanded = !diagnosticsExpanded
                updateDiagnosticsVisibility()
            }
        }

        fun resolutionLabel(value: Int): String = when (value.coerceIn(0, 2)) {
            0 -> "1/2X"
            1 -> "3/4X"
            2 -> "1X"
            else -> "?"
        }
        fun aspectLabel(value: Int): String = when (value.coerceIn(0, 4)) {
            0 -> "16:9"
            1 -> "4:3"
            2 -> "21:9"
            3 -> "16:10"
            4 -> "拉伸全屏"
            else -> "?"
        }
        fun refreshPerfButtons(
            frameButton: Button,
            resButton: Button,
            aspectButton: Button,
            graphicsCompatButton: Button,
            ncePrefButton: Button,
            perfHudPrefButton: Button
        ) {
            frameButton.text = "外部设置：跳帧 ${AppPreferences.frameSkip(this@MainActivity)}"
            resButton.text = "外部设置：分辨率 ${resolutionLabel(AppPreferences.resolutionSetup(this@MainActivity))}"
            aspectButton.text = "外部设置：画面 ${aspectLabel(AppPreferences.aspectRatio(this@MainActivity))}"
            graphicsCompatButton.text =
                if (AppPreferences.graphicsCompat(this@MainActivity)) "外部设置：图形兼容 开" else "外部设置：图形兼容 关"
            ncePrefButton.text = if (AppPreferences.preferNce(this@MainActivity)) "外部设置：NCE 请求开" else "外部设置：NCE 关"
            perfHudPrefButton.text =
                if (AppPreferences.perfHudDetailed(this@MainActivity)) "外部设置：性能HUD 详细" else "外部设置：性能HUD 精简"
        }
        lateinit var frameSkipPrefButton: Button
        lateinit var resolutionPrefButton: Button
        lateinit var aspectPrefButton: Button
        lateinit var graphicsCompatPrefButton: Button
        lateinit var ncePrefButton: Button
        lateinit var perfHudPrefButton: Button
        frameSkipPrefButton = Button(this).apply {
            setOnClickListener {
                val next = (AppPreferences.frameSkip(this@MainActivity) + 1) % 5
                AppPreferences.savePerformance(
                    this@MainActivity,
                    next,
                    AppPreferences.resolutionSetup(this@MainActivity),
                    AppPreferences.aspectRatio(this@MainActivity),
                    AppPreferences.graphicsCompat(this@MainActivity),
                    AppPreferences.preferNce(this@MainActivity)
                )
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已保存外部跳帧设置；新启动游戏最稳，运行中菜单只用于临时调试")
            }
        }
        resolutionPrefButton = Button(this).apply {
            setOnClickListener {
                val current = AppPreferences.resolutionSetup(this@MainActivity)
                val next = when (current) { 1 -> 0; 0 -> 2; else -> 1 }
                AppPreferences.savePerformance(
                    this@MainActivity,
                    AppPreferences.frameSkip(this@MainActivity),
                    next,
                    AppPreferences.aspectRatio(this@MainActivity),
                    AppPreferences.graphicsCompat(this@MainActivity),
                    AppPreferences.preferNce(this@MainActivity)
                )
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已保存外部分辨率设置；建议回菜单后重新启动游戏验证")
            }
        }
        aspectPrefButton = Button(this).apply {
            setOnClickListener {
                val current = AppPreferences.aspectRatio(this@MainActivity)
                val next = when (current) {
                    4 -> 0
                    0 -> 2
                    2 -> 3
                    3 -> 1
                    else -> 4
                }
                AppPreferences.savePerformance(
                    this@MainActivity,
                    AppPreferences.frameSkip(this@MainActivity),
                    AppPreferences.resolutionSetup(this@MainActivity),
                    next,
                    AppPreferences.graphicsCompat(this@MainActivity),
                    AppPreferences.preferNce(this@MainActivity)
                )
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已保存画面比例设置；默认拉伸全屏，切换后建议重新启动游戏验证")
            }
        }
        graphicsCompatPrefButton = Button(this).apply {
            setOnClickListener {
                val next = !AppPreferences.graphicsCompat(this@MainActivity)
                AppPreferences.savePerformance(
                    this@MainActivity,
                    AppPreferences.frameSkip(this@MainActivity),
                    AppPreferences.resolutionSetup(this@MainActivity),
                    AppPreferences.aspectRatio(this@MainActivity),
                    next,
                    AppPreferences.preferNce(this@MainActivity)
                )
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(
                    selectedGamePath ?: sampleNro.absolutePath,
                    title = if (next) "已开启图形兼容模式；建议重启游戏复测黑方块/花屏" else "已关闭图形兼容模式；恢复性能优先"
                )
            }
        }
        ncePrefButton = Button(this).apply {
            setOnClickListener {
                val next = !AppPreferences.preferNce(this@MainActivity)
                AppPreferences.savePerformance(
                    this@MainActivity,
                    AppPreferences.frameSkip(this@MainActivity),
                    AppPreferences.resolutionSetup(this@MainActivity),
                    AppPreferences.aspectRatio(this@MainActivity),
                    AppPreferences.graphicsCompat(this@MainActivity),
                    next
                )
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "已保存外部 NCE 设置；NCE 需要重新启动游戏才可靠生效")
            }
        }
        perfHudPrefButton = Button(this).apply {
            setOnClickListener {
                val next = !AppPreferences.perfHudDetailed(this@MainActivity)
                AppPreferences.savePerfHudDetailed(this@MainActivity, next)
                refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)
                refreshStatus(
                    selectedGamePath ?: sampleNro.absolutePath,
                    title = if (next) "已开启详细性能 HUD；下次启动游戏会显示 NCE/驱动/FB/present 细节" else "已切回精简性能 HUD"
                )
            }
        }
        refreshPerfButtons(frameSkipPrefButton, resolutionPrefButton, aspectPrefButton, graphicsCompatPrefButton, ncePrefButton, perfHudPrefButton)

        fun shellButton(button: Button, primary: Boolean = false) {
            button.isAllCaps = false
            button.textSize = if (primary) 17f else 14.5f
            button.setTextColor(if (primary) 0xffffffff.toInt() else 0xffe5e9ff.toInt())
            button.minHeight = 0
            button.minimumHeight = 0
            button.setPadding(18, 8, 18, 8)
            button.background = roundedBg(
                if (primary) 0xff5b5ff0.toInt() else 0xff252a36.toInt(),
                18f,
                if (primary) 0x665f7cff else 0x334b5d7a
            )
        }
        listOf(
            selectButton,
            selectFolderButton,
            scanDefaultRomButton,
            bundledButton,
            runBundledButton,
            runSelectedButton,
            runLastButton,
            gamePropertiesButton,
            allFilesAccessButton,
            autoDriverButton,
            selectDriverZipButton,
            selectDriverFolderButton,
            systemDriverButton,
            clearGraphicsCacheButton,
            autoOutputLogButton,
            frameSkipPrefButton,
            resolutionPrefButton,
            aspectPrefButton,
            graphicsCompatPrefButton,
            ncePrefButton,
            perfHudPrefButton,
            copyLogButton
        ).forEach { shellButton(it, primary = it === runSelectedButton || it === runLastButton) }
        (diagnosticsToggleButton as? Button)?.let { shellButton(it) }

        fun buttonParams(): LinearLayout.LayoutParams =
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 66).apply {
                setMargins(8, 6, 8, 6)
            }

        fun section(title: String, subtitle: String = "", vararg views: View): LinearLayout {
            return LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(16, 14, 16, 16)
                background = roundedBg(0xcc11151f.toInt(), 24f, 0x224b5d7a)
                addView(TextView(this@MainActivity).apply {
                    text = title
                    textSize = 17f
                    typeface = Typeface.DEFAULT_BOLD
                    setTextColor(0xffffffff.toInt())
                    setPadding(8, 2, 8, 0)
                })
                if (subtitle.isNotBlank()) {
                    addView(TextView(this@MainActivity).apply {
                        text = subtitle
                        textSize = 12.5f
                        setTextColor(0xffaab4d4.toInt())
                        setPadding(8, 0, 8, 8)
                    })
                }
                views.forEach { child ->
                    child.layoutParams = buttonParams()
                    addView(child)
                }
                layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(10, 10, 10, 10)
                }
            }
        }

        fun row(vararg sections: LinearLayout): LinearLayout {
            return LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.TOP
                sections.forEach { addView(it) }
            }
        }

        val hero = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 20, 24, 18)
            background = roundedBg(0xff171b2a.toInt(), 28f, 0x445f7cff)
            addView(TextView(this@MainActivity).apply {
                text = "NxEmu Android"
                textSize = 28f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xffffffff.toInt())
            })
            addView(TextView(this@MainActivity).apply {
            text = "NxEmu Android shell · nxemu core · DN​SP/DXCI focused"
                textSize = 13.5f
                setTextColor(0xffaeb8dc.toInt())
                setPadding(0, 2, 0, 0)
            })
            selectedGameView = TextView(this@MainActivity).apply {
                textSize = 13f
                setTextColor(0xffd8e2ff.toInt())
                setPadding(0, 12, 0, 0)
                text = selectedGameSummaryText()
            }
            addView(selectedGameView)
            shellSummaryView = TextView(this@MainActivity).apply {
                textSize = 12.5f
                setTextColor(0xffaeb8dc.toInt())
                setPadding(0, 8, 0, 0)
                text = shellSummaryText()
            }
            addView(shellSummaryView)
        }

        gameLibraryContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(16, 14, 16, 16)
            background = roundedBg(0xcc11151f.toInt(), 24f, 0x224b5d7a)
            addView(TextView(this@MainActivity).apply {
                text = "Game Library"
                textSize = 17f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xffffffff.toInt())
                setPadding(8, 2, 8, 0)
            })
            addView(TextView(this@MainActivity).apply {
            text = "NxEmu 游戏库：点击卡片选择，长按卡片直接启动；默认主线 .dnsp/.dxci。"
                textSize = 12.5f
                setTextColor(0xffaab4d4.toInt())
                setPadding(8, 0, 8, 8)
            })
            gameFilterInput = EditText(this@MainActivity).apply {
                hint = "搜索/过滤：如 metal、kirby、dnsp、dxci、v0"
                setSingleLine(true)
                textSize = 13.5f
                setTextColor(0xffffffff.toInt())
                setHintTextColor(0xff7f8aaa.toInt())
                setPadding(18, 8, 18, 8)
                background = roundedBg(0xff202633.toInt(), 18f, 0x334b5d7a)
                addTextChangedListener(object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                    override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                        gameFilterText = s?.toString().orEmpty()
                        updateGameLibraryUi()
                    }
                    override fun afterTextChanged(s: Editable?) = Unit
                })
            }
            addView(gameFilterInput, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 58).apply {
                setMargins(8, 4, 8, 8)
            })
        }
        updateGameLibraryUi()

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.START
            setPadding(22, 18, 22, 22)
            setBackgroundColor(SHELL_BG_COLOR)
            addView(hero, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(10, 8, 10, 10)
            })
            addView(
                row(
                    section(
                        "Library",
                        "选择、扫描并启动游戏",
                        runSelectedButton,
                        runLastButton,
                        gamePropertiesButton,
                        selectButton,
                        selectFolderButton,
                        scanDefaultRomButton
                    ),
                    section(
                        "Graphics",
                        "驱动、缓存和黑块/花屏排查",
                        autoDriverButton,
                        selectDriverZipButton,
                        selectDriverFolderButton,
                        systemDriverButton,
                        clearGraphicsCacheButton
                    )
                )
            )
            addView(
                row(
                    section(
                        "Performance",
            "NxEmu quick settings",
                        frameSkipPrefButton,
                        resolutionPrefButton,
                        aspectPrefButton,
                        graphicsCompatPrefButton,
                        ncePrefButton,
                        perfHudPrefButton
                    ),
                    section(
                        "Diagnostics",
                        "测试、日志和权限",
                        allFilesAccessButton,
                        autoOutputLogButton,
                        diagnosticsToggleButton,
                        copyLogButton,
                        bundledButton,
                        runBundledButton
                    )
                )
            )
            addView(gameLibraryContainer, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(10, 10, 10, 10)
            })
            addView(TextView(this@MainActivity).apply {
                text = "Status / Diagnostics"
                textSize = 17f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(0xffffffff.toInt())
                setPadding(18, 14, 18, 6)
            })
            addView(statusView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                setMargins(10, 0, 10, 16)
            })
            updateDiagnosticsVisibility()
        }

        return ScrollView(this).apply {
            setBackgroundColor(SHELL_BG_COLOR)
            addView(layout)
        }
    }

    private fun updateGameLibraryUi() {
        if (::selectedGameView.isInitialized) {
            selectedGameView.text = selectedGameSummaryText()
        }
        if (::shellSummaryView.isInitialized) {
            shellSummaryView.text = shellSummaryText()
        }
        if (!::gameAdapter.isInitialized) {
            return
        }

        val filter = gameFilterText.trim().lowercase(Locale.US)
        val visibleGames = sortGames(scannedGames.filter { entry ->
            filter.isBlank() ||
                entry.name.lowercase(Locale.US).contains(filter) ||
                entry.uri.toString().lowercase(Locale.US).contains(filter) ||
                entry.name.substringAfterLast('.', "").lowercase(Locale.US).contains(filter) ||
                classifyGameFile(entry.name).lowercase(Locale.US).contains(filter) ||
                extractTitleId(entry.name).orEmpty().lowercase(Locale.US).contains(filter)
        })

        binding.noticeText.visibility = View.VISIBLE
        binding.noticeText.text = when {
            scannedGames.isEmpty() -> buildString {
                appendLine("还没有扫描到游戏。")
                appendLine("推荐：把 .dnsp/.dxci 放到 /sdcard/ns/rom 或 /sdcard/ns/roms，然后点“扫描ROM”。")
                appendLine("封面：放到 /sdcard/ns/covers，文件名用 TitleID 或游戏文件名。")
                append("也可以点“授权目录”走 SAF，适合 Android 13/14 的外置目录权限。")
            }
            visibleGames.isEmpty() -> "没有匹配的游戏。可以清空搜索框，或换关键词如 dnsp、dxci、v0。"
            filter.isBlank() -> "已扫描 ${scannedGames.size} 个文件；封面 ${coverCountFor(scannedGames)}/${scannedGames.size}；排序=${sortModeLabel(homeSortMode)}；原始NSP/XCI=${if (AppPreferences.showRawSwitchContainers(this)) "显示" else "隐藏"}。"
            else -> "过滤 “$gameFilterText”：显示 ${visibleGames.size}/${scannedGames.size} 个；封面 ${coverCountFor(visibleGames)}/${visibleGames.size}；排序=${sortModeLabel(homeSortMode)}。"
        }
        gameAdapter.submit(visibleGames.take(96))
    }

    private fun gameCard(index: Int, entry: GameEntry, isSelected: Boolean): LinearLayout {
        val uriText = entry.uri.toString()
        val ext = entry.name.substringAfterLast('.', missingDelimiterValue = "?").uppercase(Locale.US)
        fun selectGame() {
            selectedGamePath = uriText
            selectedGameName = entry.name
            AppPreferences.saveLastGame(this@MainActivity, selectedGamePath, selectedGameName)
            updateGameLibraryUi()
            refreshStatus(selectedGamePath, title = "已从游戏库选择：${entry.name}")
        }
        fun smallActionButton(label: String, primary: Boolean = false, action: () -> Unit): Button =
            Button(this).apply {
                isAllCaps = false
                text = label
                textSize = 12.5f
                setTextColor(0xffffffff.toInt())
                minHeight = 0
                minimumHeight = 0
                setPadding(10, 2, 10, 2)
                background = roundedBg(if (primary) 0xff5b5ff0.toInt() else 0xff252a36.toInt(), 16f, 0x334b5d7a)
                setOnClickListener { action() }
            }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(12, 10, 12, 10)
            background = roundedBg(
                if (isSelected) 0xff24305f.toInt() else 0xaa171b26.toInt(),
                20f,
                if (isSelected) 0x887c8cff else 0x22364252
            )
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(TextView(this@MainActivity).apply {
                    text = ext.take(4)
                    textSize = 15f
                    typeface = Typeface.DEFAULT_BOLD
                    gravity = Gravity.CENTER
                    setTextColor(0xffffffff.toInt())
                    background = roundedBg(
                        when (ext.lowercase(Locale.US)) {
                            "dnsp", "dxci" -> 0xff5b5ff0.toInt()
                            "nro" -> 0xff287a64.toInt()
                            else -> 0xff596070.toInt()
                        },
                        18f,
                        0x335f7cff
                    )
                }, LinearLayout.LayoutParams(92, 92).apply {
                    setMargins(0, 0, 14, 0)
                })
                addView(TextView(this@MainActivity).apply {
                    text = buildString {
                    append(if (isSelected) "▶ " else "")
                    appendLine("${index + 1}. ${entry.name}")
                    appendLine("$ext · ${formatBytes(entry.size)} · ${classifyGameFile(entry.name)}")
                    extractTitleId(entry.name)?.let { appendLine("TitleID=$it") }
                    appendLine(AppPreferences.profileSummary(this@MainActivity, uriText))
                    append(uriText)
                }
                    textSize = 13f
                    setTextColor(if (isSelected) 0xffffffff.toInt() else 0xffd8e2ff.toInt())
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            })
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.END
                addView(smallActionButton("启动", primary = true) {
                    selectGame()
                    startEmulation(uriText)
                }, LinearLayout.LayoutParams(0, 54, 1f).apply { setMargins(0, 8, 6, 0) })
                addView(smallActionButton("属性/配置") {
                    selectGame()
                    openPerGameSettings(uriText, entry.name)
                }, LinearLayout.LayoutParams(0, 54, 1f).apply { setMargins(6, 8, 0, 0) })
            })
            setOnClickListener {
                selectGame()
            }
            setOnLongClickListener {
                selectedGamePath = uriText
                selectedGameName = entry.name
                AppPreferences.saveLastGame(this@MainActivity, selectedGamePath, selectedGameName)
                startEmulation(uriText)
                true
            }
        }
    }

    private fun selectedGameSummaryText(): String {
        val game = selectedGamePath
        val name = effectiveSelectedGameName()
        return if (game.isNullOrBlank()) {
            "当前选择：未选择游戏 · 推荐先扫描 /sdcard/ns/rom"
        } else {
            "当前选择：${name.ifBlank { "未命名" }} · ${classifyGameFile(name.ifBlank { game })}\n" +
                "${AppPreferences.profileSummary(this, game)}\n$game"
        }
    }

    private fun shellSummaryText(): String {
        return buildString {
            append("外壳：NxEmu Android · ")
            append("NCE=${if (AppPreferences.preferNce(this@MainActivity)) "请求开" else "关"} · ")
            append("跳帧=${AppPreferences.frameSkip(this@MainActivity)} · ")
            append("分辨率=${resolutionLabelForStatus(AppPreferences.resolutionSetup(this@MainActivity))} · ")
            append("画面=${aspectRatioLabelForStatus(AppPreferences.aspectRatio(this@MainActivity))} · ")
            append("HUD=${if (AppPreferences.perfHudDetailed(this@MainActivity)) "详细" else "精简"} · ")
            append("日志=${if (AppPreferences.autoOutputLog(this@MainActivity)) "自动" else "手动"} · ")
            append("视图=${bindingOrNullViewMode()} · 排序=${sortModeLabel(homeSortMode)}")
        }
    }

    private fun bindingOrNullViewMode(): String = when (homeViewMode) {
        HOME_VIEW_LIST -> "List"
        HOME_VIEW_COMPACT -> "Compact"
        else -> "Grid"
    }

    private fun updateDiagnosticsVisibility() {
        if (::binding.isInitialized) {
            binding.diagnosticsPanel.visibility = if (diagnosticsExpanded) View.VISIBLE else View.GONE
        }
        if (::statusView.isInitialized) {
            statusView.visibility = if (diagnosticsExpanded) View.VISIBLE else View.GONE
        }
        if (::diagnosticsToggleButton.isInitialized) {
            diagnosticsToggleButton.contentDescription = if (diagnosticsExpanded) "隐藏诊断详情" else "更多"
        }
    }

    private fun effectiveSelectedGameName(): String {
        val game = selectedGamePath.orEmpty()
        if (game.isBlank()) {
            return selectedGameName
        }
        val derived = deriveNameFromGamePath(game)
        if (derived.isBlank()) {
            return selectedGameName
        }
        if (selectedGameName.isBlank()) {
            return derived
        }
        // 之前多次调试中可能出现“lastGamePath 已变，但 lastGameName 仍是旧游戏”的脏偏好。
        // 对普通文件路径优先用真实文件名，避免首页显示 Kirby、实际启动 Metal Dogs 这种错配。
        val lowerGame = game.lowercase(Locale.US)
        val lowerName = selectedGameName.lowercase(Locale.US)
        return if (game.startsWith("/") && lowerName !in lowerGame) derived else selectedGameName
    }

    private fun normalizeSelectedGameName() {
        val effective = effectiveSelectedGameName()
        if (effective.isNotBlank() && effective != selectedGameName) {
            selectedGameName = effective
            selectedGamePath?.let { AppPreferences.saveLastGame(this, it, selectedGameName) }
        }
    }

    private fun deriveNameFromGamePath(game: String): String {
        return when {
            game.startsWith("/") -> game.substringAfterLast('/')
            game.startsWith("content://") -> Uri.decode(game.substringAfterLast('/')).substringAfterLast(':')
            else -> Uri.decode(game).substringAfterLast('/').substringAfterLast("%2F")
        }.orEmpty()
    }

    private fun formatBytes(size: Long): String {
        if (size <= 0L) return "未知大小"
        val units = arrayOf("B", "KB", "MB", "GB", "TB")
        var value = size.toDouble()
        var unit = 0
        while (value >= 1024.0 && unit < units.lastIndex) {
            value /= 1024.0
            unit++
        }
        return if (unit == 0) {
            "${size}B"
        } else {
            String.format(Locale.US, "%.2f%s", value, units[unit])
        }
    }

    private fun roundedBg(color: Int, radius: Float, strokeColor: Long = 0L): GradientDrawable {
        return GradientDrawable().apply {
            setColor(color)
            cornerRadius = radius
            if (strokeColor != 0L) {
                setStroke(1, strokeColor.toInt())
            }
        }
    }

    private fun openGamePicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
            putExtra(
                Intent.EXTRA_MIME_TYPES,
                arrayOf(
                    "application/octet-stream",
                    "application/x-nintendo-switch-nro",
                    "application/x-nintendo-switch-nsp",
                    "application/x-nintendo-switch-xci",
                    "application/x-nxemu-dnsp",
                    "application/x-nxemu-dxci"
                )
            )
        }
        startActivityForResult(intent, REQUEST_OPEN_GAME)
    }

    private fun openGameFolderPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        }
        startActivityForResult(intent, REQUEST_OPEN_GAME_FOLDER)
    }

    private fun openDriverZipPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/octet-stream"))
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
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()) {
            Toast.makeText(this, "已具备直接读取外置存储权限", Toast.LENGTH_LONG).show()
            refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "外置存储权限已可用")
            return
        }
        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
            data = Uri.parse("package:$packageName")
        }
        runCatching { startActivity(intent) }.onFailure {
            startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    private fun persistReadPermission(uri: Uri, resultFlags: Int) {
        val flags = resultFlags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        val readFlags = if (flags == 0) Intent.FLAG_GRANT_READ_URI_PERMISSION else flags
        runCatching { contentResolver.takePersistableUriPermission(uri, readFlags) }
    }

    private fun refreshStatus(game: String?, title: String) {
        if (::selectedGameView.isInitialized) {
            selectedGameView.text = selectedGameSummaryText()
        }
        if (::shellSummaryView.isInitialized) {
            shellSummaryView.text = shellSummaryText()
        }
        statusView.text = buildString {
            appendLine("NxEmu Android")
            appendLine(title)
            appendLine()
            appendLine("最近加载/设置：")
            appendLine("lastGameName=${AppPreferences.lastGameName(this@MainActivity).ifBlank { "none" }}")
            appendLine("lastGame=${AppPreferences.lastGamePath(this@MainActivity).ifBlank { "none" }}")
            appendLine("lastFolder=${AppPreferences.lastGameFolder(this@MainActivity).ifBlank { "none" }}")
            appendLine("lastFrameSkip=${AppPreferences.frameSkip(this@MainActivity)}")
            appendLine("lastResolution=${resolutionLabelForStatus(AppPreferences.resolutionSetup(this@MainActivity))}")
            appendLine("lastNce=${AppPreferences.preferNce(this@MainActivity)}")
            appendLine("lastPerfHudDetailed=${AppPreferences.perfHudDetailed(this@MainActivity)}")

            runCatching {
                appendLine("native=${NativeLibrary.version()}")
                appendLine("initialized=${NativeLibrary.initialize(NativeLibrary::class.java)}")
                appendLine(GpuDriverHelper.initialize(this@MainActivity))
                appendLine(GpuDriverHelper.summaryText())
                File(filesDir, "user").mkdirs()
                appendLine(NativeLibrary.initializeRuntime(filesDir.absolutePath, applicationInfo.nativeLibraryDir))
            }.onFailure { error ->
                appendLine("native load failed:")
                appendLine(error.stackTraceToString())
                return@buildString
            }

            appendLine()
            appendLine("Core module load:")
            NativeLibrary.loadCoreModules().forEach { appendLine("- $it") }
            appendLine()
            appendLine(AppPreferences.statusText(this@MainActivity))

            appendLine()
            if (game == null) {
                appendLine("game=none")
            } else {
                if (selectedFolderPath.isNotBlank()) {
                    appendLine("authorizedFolder=$selectedFolderPath")
                    appendLine("scannedGames=${scannedGames.size}")
                    scannedGames.take(12).forEachIndexed { index, entry ->
                        appendLine("[$index] ${entry.name} size=${entry.size} ${classifyGameFile(entry.name)}")
                        appendLine("    ${entry.uri}")
                    }
                    if (scannedGames.size > 12) {
                        appendLine("... ${scannedGames.size - 12} more")
                    }
                    appendLine()
                }
                appendLine("game=$game")
                if (selectedGameName.isNotBlank()) {
                    appendLine("displayName=$selectedGameName")
                    appendLine("gameKind=${classifyGameFile(selectedGameName)}")
                }
                appendLine("exists=${NativeLibrary.exists(game)}")
                appendLine("size=${NativeLibrary.getSize(game)}")
                appendLine()
                appendLine(NativeLibrary.probeGame(game))
            }

            appendLine()
            appendLine("目标主线：直接运行 NxEmu 原生 .dxci/.dnsp；.nro 只作为测试/homebrew。")
        appendLine("文件读取：外置目录用 SAF 授权，native 通过 content:// fd 读取，不走 /sdcard fopen。")
            appendLine("格式说明：.dxci 按 XCI loader，.dnsp 按 NSP loader，.nro 按 homebrew loader。")
            appendLine("当前重点：DXCI/DNSP loader 启动、Android Vulkan/Turnip 驱动、Surface、输入、音频、存档。")
            appendLine()
            appendLine(GpuDriverHelper.statusText())
            appendLine()
            appendLine("现在可以测试：点“授权外置游戏目录”或“选择 DXCI/DNSP 游戏或 NRO”，然后运行已选择游戏。")
            appendLine("如果 LoadRom failed，复制运行页日志给我，我会按 loader 阶段继续补。")
        }
    }

    private fun resolutionLabelForStatus(value: Int): String = when (value.coerceIn(0, 2)) {
        0 -> "1/2X"
        1 -> "3/4X"
        2 -> "1X"
        else -> "?"
    }

    private fun aspectRatioLabelForStatus(value: Int): String = when (value.coerceIn(0, 4)) {
        0 -> "16:9"
        1 -> "4:3"
        2 -> "21:9"
        3 -> "16:10"
        4 -> "拉伸"
        else -> "?"
    }

    private fun ensureBundledSample(forceCopy: Boolean = false): File {
        if (forceCopy || !sampleNro.exists()) {
            copyAsset("hbmenu.nro", sampleNro)
        }

        // libnx romfsMountSelf() opens argv[0]. The Android PoC gives hbmenu
        // argv0=sdmc:/hbmenu.nro, so mirror the built-in NRO into the emulated SDMC root.
        val sdmcSample = File(filesDir, "user/sdmc/hbmenu.nro")
        if (forceCopy || !sdmcSample.exists() || sdmcSample.length() != sampleNro.length()) {
            copyAsset("hbmenu.nro", sdmcSample)
        }

        val sdmcSwitchSample = File(filesDir, "user/sdmc/switch/000-hbmenu-test.nro")
        if (forceCopy || !sdmcSwitchSample.exists() || sdmcSwitchSample.length() != sampleNro.length()) {
            copyAsset("hbmenu.nro", sdmcSwitchSample)
        }

        // Remove an older test NRO that hbmenu marks as ABI-incompatible. Keeping it as
        // the first entry makes A/X look broken because hbmenu refuses to write NextLoad.
        File(filesDir, "user/sdmc/switch/nton-sdl-hello.nro").delete()
        return sampleNro
    }

    private fun copyAsset(assetName: String, destination: File) {
        destination.parentFile?.mkdirs()
        assets.open(assetName).use { input ->
            destination.outputStream().use { output ->
                input.copyTo(output)
            }
        }
    }

    private fun copyOptionalAsset(assetName: String, destination: File, forceCopy: Boolean = false) {
        runCatching {
            assets.open(assetName).use { input ->
                if (!forceCopy && destination.exists() && destination.length() > 0) {
                    return
                }
                destination.parentFile?.mkdirs()
                destination.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }
    }

    private fun importGame(uri: Uri): String {
        // DXCI/DNSP are usually multi-GB. Keep SAF URI instead of copying the whole file.
        // Native VFS uses NativeLibrary.openContentUri() to read content:// directly.
        return uri.toString()
    }

    private fun queryDisplayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null).use { cursor ->
            if (cursor != null && cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) {
                    return cursor.getString(index).orEmpty()
                }
            }
        }
        return uri.lastPathSegment.orEmpty()
    }

    private fun scanGameFolder(rootUri: Uri, maxDepth: Int = 3): List<GameEntry> {
        val results = mutableListOf<GameEntry>()

        fun scan(directoryUri: Uri, depth: Int) {
            if (depth <= 0) {
                return
            }

            val docId = if (isRootTreeUri(directoryUri)) {
                DocumentsContract.getTreeDocumentId(directoryUri)
            } else {
                DocumentsContract.getDocumentId(directoryUri)
            }
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(directoryUri, docId)
            val columns = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE
            )

            contentResolver.query(childrenUri, columns, null, null, null).use { cursor ->
                if (cursor == null) {
                    return
                }
                val idIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val mimeIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
                val sizeIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_SIZE)
                while (cursor.moveToNext()) {
                    val childId = cursor.getString(idIndex)
                    val name = cursor.getString(nameIndex).orEmpty()
                    val mime = cursor.getString(mimeIndex).orEmpty()
                    val size = if (cursor.isNull(sizeIndex)) 0L else cursor.getLong(sizeIndex)
                    val childUri = DocumentsContract.buildDocumentUriUsingTree(rootUri, childId)
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                        scan(childUri, depth - 1)
                    } else if (shouldShowInGameLibrary(name)) {
                        results += GameEntry(name, childUri, size)
                    }
                }
            }
        }

        scan(rootUri, maxDepth)
        return dedupeGameEntries(results)
    }

    private fun isRootTreeUri(uri: Uri): Boolean {
        val segments = uri.pathSegments
        return segments.size == 2 && segments[0] == "tree"
    }

    private fun isSupportedGameName(name: String): Boolean {
        return GamePathResolver.isSupportedGameName(name)
    }

    private fun shouldShowInGameLibrary(name: String): Boolean {
        if (!isSupportedGameName(name)) {
            return false
        }
        if (AppPreferences.showRawSwitchContainers(this)) {
            return true
        }
        val ext = name.substringAfterLast('.', missingDelimiterValue = "").lowercase(Locale.US)
        return ext !in setOf("nsp", "xci")
    }

    private fun scanDefaultNsRomFolder(): List<GameEntry> {
        val roots = GamePathResolver.defaultRomRoots(this)
            .filter { it.absolutePath.contains("/ns/rom") }
            .distinctBy { it.absolutePath }
        val readableRoots = roots.filter { it.exists() && it.canRead() }
        if (readableRoots.isEmpty()) {
            Toast.makeText(this, "/sdcard/ns/rom 或 /sdcard/ns/roms 不可读，先点“授权所有文件访问/外置驱动”", Toast.LENGTH_LONG).show()
            return emptyList()
        }
        return readableRoots
            .asSequence()
            .flatMap { root ->
                root.walkTopDown()
                    .maxDepth(2)
                    .filter { it.isFile && shouldShowInGameLibrary(it.name) }
                    .map { GameEntry(it.name, Uri.parse(it.absolutePath), it.length()) }
            }
            .let { dedupeGameEntries(it.toList()).asSequence() }
            .sortedWith(
                compareByDescending<GameEntry> { classifyGameScore(it.name) }
                    .thenBy { it.name.lowercase(Locale.US) }
            )
            .toList()
    }

    private fun dedupeGameEntries(entries: List<GameEntry>): List<GameEntry> {
        val byPhysicalPath = linkedMapOf<String, GameEntry>()
        entries.forEach { entry ->
            val key = normalizeGamePathKey(entry.uri.toString()).ifBlank { entry.uri.toString() }
            val old = byPhysicalPath[key]
            if (old == null || gameEntryPreferenceScore(entry) > gameEntryPreferenceScore(old)) {
                byPhysicalPath[key] = entry
            }
        }

        val byGameIdentity = linkedMapOf<String, GameEntry>()
        byPhysicalPath.values.forEach { entry ->
            val identity = gameIdentityKey(entry)
            val old = byGameIdentity[identity]
            if (old == null || gameEntryPreferenceScore(entry) > gameEntryPreferenceScore(old)) {
                byGameIdentity[identity] = entry
            }
        }

        return byGameIdentity.values.sortedWith(
            compareByDescending<GameEntry> { classifyGameScore(it.name) }
                .thenBy { it.name.lowercase(Locale.US) }
        )
    }

    private fun gameEntryPreferenceScore(entry: GameEntry): Int {
        val lower = entry.name.lowercase(Locale.US)
        val ext = lower.substringAfterLast('.', "")
        var score = classifyGameScore(entry.name)
        score += when (ext) {
            "dnsp" -> 500
            "dxci" -> 480
            "nro" -> 180
            "nca" -> 80
            "nsp" -> 20
            "xci" -> 10
            else -> 0
        }
        if (entry.size > 0L) score += 5
        if ("/sdcard/ns/rom/" in normalizeGamePathKey(entry.uri.toString())) score += 3
        return score
    }

    private fun gameIdentityKey(entry: GameEntry): String {
        extractTitleId(entry.name)?.let { return "title:$it" }
        val base = entry.name.substringBeforeLast('.', entry.name)
            .replace(Regex("""\[[^]]*]"""), " ")
            .replace(Regex("""\([^)]*\)"""), " ")
            .replace(Regex("""\bv\d+(\.\d+)*\b""", RegexOption.IGNORE_CASE), " ")
        return "name:${sanitizeCoverKey(base)}"
    }

    private fun normalizeGamePathKey(raw: String): String {
        val normalized = GamePathResolver.normalize(raw).replace('\\', '/')
        if (normalized.startsWith("content://", ignoreCase = true)) {
            return normalized
                .substringBefore('?')
                .substringBefore('#')
                .lowercase(Locale.US)
        }
        val path = normalized
            .replaceFirst(Regex("""^/storage/emulated/0/"""), "/sdcard/")
            .replace(Regex("""/+"""), "/")
        return runCatching { File(path).canonicalPath.replace('\\', '/') }
            .getOrDefault(path)
            .replaceFirst(Regex("""^/storage/emulated/0/"""), "/sdcard/")
            .lowercase(Locale.US)
    }

    private fun classifyGameFile(name: String): String {
        val lower = name.lowercase(Locale.US)
        val ext = lower.substringAfterLast('.', missingDelimiterValue = "")
        val isUpdateOrDlc = listOf("update", "dlc", "补丁", "升级", "1u", "2u", "3d", "18d").any { it in lower } ||
            Regex("""\[v(?!0\])\d+""").containsMatchIn(lower) ||
            Regex("""v1\.\d""").containsMatchIn(lower)
        val baseHint = when {
            "[v0]" in lower || "v0]" in lower -> "本体概率高"
            isUpdateOrDlc -> "可能是更新/DLC/整合包，单独运行可能黑屏或失败"
            ext == "nro" -> "homebrew测试程序"
            ext == "dnsp" || ext == "dxci" -> "NxEmu预处理格式"
            ext == "nsp" || ext == "xci" -> "原始Switch容器，可能需要keys/解密链路"
            else -> "未知类型"
        }
        return "[$baseHint]"
    }

    private fun findGameCover(entry: GameEntry): File? {
        val titleId = extractTitleId(entry.name)
        val baseName = entry.name.substringBeforeLast('.', entry.name)
        val normalizedBase = sanitizeCoverKey(baseName)
        val candidates = mutableListOf<String>()
        if (!titleId.isNullOrBlank()) {
            candidates += titleId
            candidates += titleId.lowercase(Locale.US)
        }
        candidates += normalizedBase
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
            root.listFiles()?.firstOrNull { file ->
                file.isFile && file.length() > 0 && file.extension.lowercase(Locale.US) in extensions &&
                    candidates.any { key -> sanitizeCoverKey(file.nameWithoutExtension).equals(key, ignoreCase = true) }
            }?.let { return it }
        }
        return null
    }

    private fun sanitizeCoverKey(text: String): String {
        return text.lowercase(Locale.US)
            .replace(Regex("""\.(dnsp|dxci|nsp|xci|nro)$"""), "")
            .replace(Regex("""[^a-z0-9]+"""), "_")
            .trim('_')
    }

    private fun extractTitleId(text: String): String? =
        Regex("""0100[0-9a-fA-F]{12}""").find(text)?.value?.uppercase(Locale.US)

    private fun formatSortScore(name: String): Int {
        val lower = name.lowercase(Locale.US)
        val ext = lower.substringAfterLast('.', "")
        return when (ext) {
            "dnsp" -> 50
            "dxci" -> 45
            "nro" -> 20
            "nsp", "xci" -> 10
            else -> 0
        } + if ("[v0]" in lower || "v0]" in lower) 5 else 0
    }

    private fun classifyGameScore(name: String): Int {
        val lower = name.lowercase(Locale.US)
        var score = 0
        if (lower.endsWith(".dnsp") || lower.endsWith(".dxci")) score += 500
        if ("[v0]" in lower || "v0]" in lower) score += 300
        if ("update" in lower || "dlc" in lower || "补丁" in lower || "升级" in lower || "1u" in lower || "2u" in lower || "3d" in lower) score -= 500
        if (Regex("""\[v(?!0\])\d+""").containsMatchIn(lower)) score -= 300
        // 小游戏/小文件优先，避免第一次测试等待大型游戏。
        return score
    }

    private fun startEmulation(game: String) {
        if (selectedGamePath != game) {
            selectedGamePath = game
            selectedGameName = deriveNameFromGamePath(game)
        }
        normalizeSelectedGameName()
        val effectiveName = effectiveSelectedGameName()
        val profile = AppPreferences.perGameProfile(this, game)
        AppPreferences.saveLastGame(this, game, effectiveName)
        startActivity(
            Intent(this, EmulationActivity::class.java).apply {
                putExtra(EmulationActivity.EXTRA_GAME_PATH, game)
                putExtra(EmulationActivity.EXTRA_GAME_NAME, effectiveName)
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
                    putExtra(EmulationActivity.EXTRA_AUTO_OUTPUT_LOG, AppPreferences.autoOutputLog(this@MainActivity))
                }
            }
        )
    }

    private fun openDriverManager() {
        startActivity(Intent(this, DriverManagerActivity::class.java))
    }

    private fun openSettings() {
        startActivity(Intent(this, SettingsActivity::class.java))
    }

    private fun openInputSettings() {
        startActivity(Intent(this, InputSettingsActivity::class.java))
    }

    private fun openHomeMenu() {
        startActivity(
            Intent(this, HomeMenuActivity::class.java).apply {
                putExtra(PerGameSettingsActivity.EXTRA_GAME_PATH, selectedGamePath.orEmpty())
                putExtra(PerGameSettingsActivity.EXTRA_GAME_NAME, effectiveSelectedGameName())
                putExtra("org.nxemu.app.EXTRA_SCANNED_COUNT", scannedGames.size)
            }
        )
    }

    private fun refreshCoversAndLibrary() {
        updateGameLibraryUi()
        val count = coverCountFor(scannedGames)
        refreshStatus(selectedGamePath ?: sampleNro.absolutePath, title = "封面缓存已刷新：$count/${scannedGames.size}")
        Toast.makeText(this, "封面已刷新：$count/${scannedGames.size}，目录 /sdcard/ns/covers", Toast.LENGTH_LONG).show()
    }

    private fun coverCountFor(entries: List<GameEntry>): Int = entries.count { findGameCover(it) != null }

    private fun openPerGameSettings(game: String, name: String) {
        startActivity(
            Intent(this, PerGameSettingsActivity::class.java).apply {
                putExtra(PerGameSettingsActivity.EXTRA_GAME_PATH, game)
                putExtra(PerGameSettingsActivity.EXTRA_GAME_NAME, name.ifBlank { deriveNameFromGamePath(game) })
            }
        )
    }

    private fun copyDiagnosticsToClipboard() {
        val selected = selectedGamePath
        val text = buildString {
            appendLine("===== NxEmu Android Main Diagnostics =====")
            appendLine("time=${SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS Z", Locale.US).format(Date())}")
            appendLine("package=$packageName")
            appendLine(
                "appVersion=${
                    runCatching { packageManager.getPackageInfo(packageName, 0).versionName }
                        .getOrDefault("unknown")
                }"
            )
            appendLine("device=${Build.MANUFACTURER} ${Build.MODEL}")
            appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT}")
            appendLine("supportedAbis=${Build.SUPPORTED_ABIS.joinToString()}")
            appendLine("nativeLibraryDir=${applicationInfo.nativeLibraryDir}")
            appendLine("sampleNro=${sampleNro.absolutePath}")
            appendLine("sampleExists=${sampleNro.exists()}")
            appendLine("sampleSize=${sampleNro.length()}")
            appendLine("selectedGame=${selected ?: "none"}")
            appendLine("selectedName=$selectedGameName")
            appendLine("authorizedFolder=${selectedFolderPath.ifBlank { "none" }}")
            appendLine("scannedGames=${scannedGames.size}")
            appendLine()
            appendLine(AppPreferences.statusText(this@MainActivity))
            appendLine(GpuDriverHelper.statusText())
            appendLine("selectedExists=${selected?.let { NativeLibrary.exists(it) } ?: false}")
            appendLine("selectedSize=${selected?.let { NativeLibrary.getSize(it) } ?: 0}")
            appendLine()
            appendLine("----- Native runtimeStatus -----")
            appendLine(runCatching { NativeLibrary.runtimeStatus() }.getOrElse { it.stackTraceToString() })
            appendLine()
            appendLine("----- Page text -----")
            appendLine(statusView.text.toString())
            appendLine("===== End NxEmu Android Main Diagnostics =====")
        }

        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("nxemu diagnostics", text))
        Toast.makeText(this, "首页诊断日志已复制，可直接粘贴发我", Toast.LENGTH_LONG).show()
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).roundToInt()

    private inner class EdenGameAdapter(
        private val onSelect: (GameEntry) -> Unit,
        private val onLaunch: (GameEntry) -> Unit,
        private val onProperties: (GameEntry) -> Unit,
        private val isSelected: (GameEntry) -> Boolean
    ) : RecyclerView.Adapter<EdenGameAdapter.GameViewHolder>() {
        private val games = mutableListOf<GameEntry>()
        private var viewMode: Int = HOME_VIEW_GRID

        fun setViewMode(mode: Int) {
            val normalized = mode.coerceIn(0, 2)
            if (viewMode != normalized) {
                viewMode = normalized
                notifyDataSetChanged()
            }
        }

        fun submit(items: List<GameEntry>) {
            val diff = DiffUtil.calculateDiff(object : DiffUtil.Callback() {
                override fun getOldListSize(): Int = games.size
                override fun getNewListSize(): Int = items.size
                override fun areItemsTheSame(oldItemPosition: Int, newItemPosition: Int): Boolean {
                    return games[oldItemPosition].uri == items[newItemPosition].uri
                }
                override fun areContentsTheSame(oldItemPosition: Int, newItemPosition: Int): Boolean {
                    val old = games[oldItemPosition]
                    val next = items[newItemPosition]
                    return old == next && isSelected(old) == isSelected(next)
                }
            })
            games.clear()
            games.addAll(items)
            diff.dispatchUpdatesTo(this)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): GameViewHolder {
            return GameViewHolder(
                ItemGameCardBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            )
        }

        override fun getItemCount(): Int = games.size

        override fun onBindViewHolder(holder: GameViewHolder, position: Int) {
            holder.bind(games[position], position)
        }

        inner class GameViewHolder(private val item: ItemGameCardBinding) : RecyclerView.ViewHolder(item.root) {
            fun bind(entry: GameEntry, index: Int) {
                val selected = isSelected(entry)
                val ext = entry.name.substringAfterLast('.', missingDelimiterValue = "?").uppercase(Locale.US)
                item.cardRoot.setBackgroundResource(if (selected) R.drawable.eden_game_card_selected else R.drawable.eden_game_card)
                val cover = findGameCover(entry)
                if (cover != null) {
                    item.gameCover.visibility = View.VISIBLE
                    item.gameCover.setImageURI(Uri.fromFile(cover))
                    item.imageBadge.setBackgroundColor(0x33000000)
                    item.imageBadge.text = ext.take(4)
                } else {
                    item.gameCover.visibility = View.GONE
                    item.gameCover.setImageDrawable(null)
                    item.imageBadge.setBackgroundResource(R.drawable.eden_badge)
                    item.imageBadge.text = ext.take(4)
                }
                item.gameTitle.text = entry.name
                item.gameDetails.text = buildString {
                    append("$ext · ${formatBytes(entry.size)} · ${classifyGameFile(entry.name)}")
                    extractTitleId(entry.name)?.let { append(" · $it") }
                }
                item.gameProfile.text = AppPreferences.profileSummary(this@MainActivity, entry.uri.toString())
                applyModeLayout(item)
                item.cardRoot.setOnClickListener { onSelect(entry) }
                item.cardRoot.setOnLongClickListener { onLaunch(entry); true }
                item.buttonStart.setOnClickListener { onLaunch(entry) }
                item.buttonProperties.setOnClickListener { onProperties(entry) }
            }

            private fun applyModeLayout(item: ItemGameCardBinding) {
                when (viewMode) {
                    HOME_VIEW_LIST -> {
                        item.contentRow.orientation = LinearLayout.HORIZONTAL
                        item.coverFrame.layoutParams = item.coverFrame.layoutParams.apply { width = dp(66); height = dp(66) }
                        item.gameTextContainer.layoutParams = LinearLayout.LayoutParams(
                            0,
                            LinearLayout.LayoutParams.WRAP_CONTENT,
                            1f
                        ).apply {
                            marginStart = dp(12)
                        }
                        item.gameTitle.textSize = 14f
                        item.gameDetails.visibility = View.VISIBLE
                        item.gameDetails.textSize = 12f
                        item.gameProfile.visibility = View.GONE
                        item.actionsRow.visibility = View.VISIBLE
                    }
                    HOME_VIEW_COMPACT -> {
                        item.contentRow.orientation = LinearLayout.VERTICAL
                        item.coverFrame.layoutParams = item.coverFrame.layoutParams.apply { width = dp(106); height = dp(106) }
                        item.gameTextContainer.layoutParams = LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT
                        ).apply {
                            topMargin = dp(2)
                            marginStart = 0
                        }
                        item.gameTitle.textSize = 12f
                        item.gameDetails.visibility = View.GONE
                        item.gameProfile.visibility = View.GONE
                        item.actionsRow.visibility = View.GONE
                    }
                    else -> {
                        item.contentRow.orientation = LinearLayout.VERTICAL
                        item.coverFrame.layoutParams = item.coverFrame.layoutParams.apply { width = dp(138); height = dp(138) }
                        item.gameTextContainer.layoutParams = LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT
                        ).apply {
                            topMargin = dp(2)
                            marginStart = 0
                        }
                        item.gameTitle.textSize = 14f
                        item.gameDetails.visibility = View.GONE
                        item.gameProfile.visibility = View.GONE
                        item.actionsRow.visibility = View.GONE
                    }
                }
            }
        }
    }

    companion object {
        private const val REQUEST_OPEN_GAME = 1001
        private const val REQUEST_OPEN_GAME_FOLDER = 1002
        private const val REQUEST_OPEN_DRIVER_ZIP = 1003
        private const val REQUEST_OPEN_DRIVER_FOLDER = 1004
        private val SHELL_BG_COLOR = 0xff090b10.toInt()
        private const val HOME_VIEW_GRID = 0
        private const val HOME_VIEW_LIST = 1
        private const val HOME_VIEW_COMPACT = 2
        private const val HOME_SORT_RECOMMENDED = 0
        private const val HOME_SORT_NAME_ASC = 1
        private const val HOME_SORT_NAME_DESC = 2
        private const val HOME_SORT_SIZE_DESC = 3
        private const val HOME_SORT_SIZE_ASC = 4
        private const val HOME_SORT_FORMAT = 5

        const val EXTRA_HOME_ACTION = "org.nxemu.app.EXTRA_HOME_ACTION"
        const val HOME_ACTION_OPEN_FILE = "open_file"
        const val HOME_ACTION_OPEN_FOLDER = "open_folder"
        const val HOME_ACTION_SCAN_DEFAULT = "scan_default"
        const val HOME_ACTION_RUN_LAST = "run_last"
        const val HOME_ACTION_REFRESH_COVERS = "refresh_covers"
        const val HOME_ACTION_COPY_LOG = "copy_log"
        const val HOME_ACTION_ALL_FILES = "all_files"
    }

    private data class GameEntry(
        val name: String,
        val uri: Uri,
        val size: Long
    )
}




