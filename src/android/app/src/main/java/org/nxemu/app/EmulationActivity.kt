package org.nxemu.app

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.os.Looper
import android.os.Handler
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.MotionEvent
import android.view.KeyEvent
import android.view.InputDevice
import android.view.Gravity
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import android.util.Log
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import kotlin.math.hypot
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

class EmulationActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var statusView: TextView
    private lateinit var inputStateView: TextView
    private lateinit var perfView: TextView
    private lateinit var frameSkipButton: Button
    private lateinit var resolutionButton: Button
    private lateinit var aspectRatioButton: Button
    private lateinit var graphicsCompatButton: Button
    private lateinit var nceButton: Button
    private lateinit var perfHudButton: Button
    private lateinit var stopButton: Button
    private lateinit var touchToggleButton: Button
    private lateinit var controlPanel: LinearLayout
    private lateinit var overlayScroll: ScrollView
    private lateinit var toggleOverlayButton: Button
    private val touchControlViews = mutableListOf<View>()
    private var touchControlsVisible = true
    private var touchOpacityScale = 0.70f
    private var touchLayoutPreset = 1
    private var gamePath: String = ""
    private var originalGamePath: String = ""
    private var gameName: String = ""
    private var gpuDriverSource: String = ""
    private var gamePathRepairStatus: String = "not-run"
    private var bootStarted = false
    private var frameSkip = 0
    private var resolutionSetup = 0
    private var aspectRatio = 4
    private var graphicsCompat = false
    private var preferNce = false
    private var perfHudDetailed = false
    private var lastNceActual = false
    private var autoOutputLog = false
    private var sessionLogFile: File? = null
    private val sessionLogLock = Any()
    private val activityStartMs = System.currentTimeMillis()
    private var lastOverlaySampleMs = activityStartMs
    private var lastVulkanPresentCount = 0.0
    private var lastVulkanCompositeCount = 0.0
    private var lastSafeRenderFps = 0.0
    private var lastSafeCompositeFps = 0.0
    private var lastNonZeroRenderFps = 0.0
    private var lastNonZeroCompositeFps = 0.0
    private var lastFrameCounterMs = activityStartMs
    private var inferredTargetFps = 60.0
    private var target30SampleCount = 0
    private var target60SampleCount = 0
    private var perfTickRound = 0
    private var lastStallLoggedPresent = -1.0
    private var lastStallLoggedComposite = -1.0
    private var renderStallSinceMs = 0L
    private var cachedTemperatureText = "--"
    private var lastTemperatureReadMs = 0L
    private var lastAnalogSessionLogMs = 0L
    private var lastBackPressMs = 0L
    @Volatile
    private var lastSavedLogPath: String = ""
    private val mainHandler = Handler(Looper.getMainLooper())
    private val hideSystemBarsRunnable = Runnable { hideSystemBars() }
    private val perfTicker = object : Runnable {
        override fun run() {
            val stats = runCatching { NativeLibrary.getPerformanceStats() }
                .getOrElse { "performanceStats=failed\n${it::class.java.simpleName}:${it.message}" }
            maybeLogPerfSnapshotAndStall(stats)
            updatePerformanceOverlay(stats)
            if (!stopRequested.get()) {
                mainHandler.postDelayed(this, 1000)
            }
        }
    }
    private val stopRequested = AtomicBoolean(false)
    @Volatile
    private var bootThread: Thread? = null
    @Volatile
    private var diagnosticThread: Thread? = null
    @Volatile
    private var performanceThread: Thread? = null
    @Volatile
    private var surfaceAlive = false
    private val debugInputReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != ACTION_DEBUG_INPUT) return
            val buttonName = intent.getStringExtra(EXTRA_DEBUG_BUTTON).orEmpty()
            val holdMs = intent.getLongExtra(EXTRA_DEBUG_HOLD_MS, 120L).coerceIn(20L, 2000L)
            val analog = debugAnalogFromName(buttonName)
            if (analog != null) {
                appendAnalogResult(analog.label, analog.stick, analog.x, analog.y)
                mainHandler.postDelayed({
                    appendAnalogResult("${analog.label}-release", analog.stick, 0f, 0f)
                }, holdMs)
                return
            }
            val ordinal = switchButtonOrdinal(buttonName)
            if (ordinal < 0) {
                appendSessionLog("debug-input-invalid", "button=$buttonName")
                return
            }
            appendInputResult(buttonName.uppercase(Locale.US), ordinal, true)
            mainHandler.postDelayed({ appendInputResult(buttonName.uppercase(Locale.US), ordinal, false) }, holdMs)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
        }
        registerDebugInputReceiver()
        originalGamePath = intent.getStringExtra(EXTRA_GAME_PATH).orEmpty()
        if (originalGamePath.isBlank()) {
            originalGamePath = AppPreferences.lastGamePath(this)
        }
        val resolvedGame = GamePathResolver.resolve(this, originalGamePath)
        gamePath = resolvedGame.path
        gameName = intent.getStringExtra(EXTRA_GAME_NAME).orEmpty()
            .ifBlank { AppPreferences.lastGameName(this) }
        gpuDriverSource = intent.getStringExtra(EXTRA_GPU_DRIVER_SOURCE).orEmpty()
        touchControlsVisible = AppPreferences.touchInitialVisible(this)
        touchOpacityScale = (AppPreferences.touchOpacity(this).coerceIn(30, 100) / 100f)
        touchLayoutPreset = AppPreferences.touchLayoutPreset(this)
        gamePathRepairStatus =
            "original=${resolvedGame.original}\nresolved=${resolvedGame.path}\nrepaired=${resolvedGame.repaired}\nreason=${resolvedGame.reason}"
        Log.i(
            "NxEmuAndroid",
            "pathRepair original=${resolvedGame.original} resolved=${resolvedGame.path} " +
                "repaired=${resolvedGame.repaired} reason=${resolvedGame.reason}"
        )
        frameSkip = if (intent.hasExtra(EXTRA_FRAME_SKIP)) {
            intent.getIntExtra(EXTRA_FRAME_SKIP, DEFAULT_FRAME_SKIP)
        } else {
            AppPreferences.frameSkip(this)
        }.coerceIn(0, 4)
        resolutionSetup = if (intent.hasExtra(EXTRA_RESOLUTION_SETUP)) {
            intent.getIntExtra(EXTRA_RESOLUTION_SETUP, 0)
        } else {
            AppPreferences.resolutionSetup(this)
        }.coerceIn(0, 2)
        aspectRatio = if (intent.hasExtra(EXTRA_ASPECT_RATIO)) {
            intent.getIntExtra(EXTRA_ASPECT_RATIO, 4)
        } else {
            AppPreferences.aspectRatio(this)
        }.coerceIn(0, 4)
        graphicsCompat = if (intent.hasExtra(EXTRA_GRAPHICS_COMPAT)) {
            intent.getBooleanExtra(EXTRA_GRAPHICS_COMPAT, false)
        } else {
            AppPreferences.graphicsCompat(this)
        }
        preferNce = if (intent.hasExtra(EXTRA_PREFER_NCE)) {
            intent.getBooleanExtra(EXTRA_PREFER_NCE, false)
        } else {
            AppPreferences.preferNce(this)
        }
        val autoProfileNote = applyPerGameCompatibilityProfile(
            allowGraphicsCompatOverride = !intent.hasExtra(EXTRA_GRAPHICS_COMPAT)
        )
        perfHudDetailed = if (intent.hasExtra(EXTRA_PERF_HUD_DETAILED)) {
            intent.getBooleanExtra(EXTRA_PERF_HUD_DETAILED, false)
        } else {
            AppPreferences.perfHudDetailed(this)
        }
        autoOutputLog = intent.getBooleanExtra(EXTRA_AUTO_OUTPUT_LOG, AppPreferences.autoOutputLog(this))
        if (autoOutputLog) {
            sessionLogFile = createSessionLogFile()
            appendSessionLog(
                "activity-create",
                buildString {
                    appendLine("autoOutputLog=true")
                    appendLine("sessionLogFile=${sessionLogFile?.absolutePath ?: "create-failed"}")
                    appendLine("originalGamePath=$originalGamePath")
                    appendLine("gamePath=$gamePath")
                    appendLine("gameName=$gameName")
                    appendLine("gpuDriverSource=${gpuDriverSource.ifBlank { "global" }}")
                    appendLine("pathRepair:")
                    appendLine(gamePathRepairStatus)
                    appendLine("perGameProfile=$autoProfileNote")
                }
            )
        }
        if (gamePath.isNotBlank()) {
            AppPreferences.saveLastGame(this, gamePath, gameName.ifBlank { AppPreferences.lastGameName(this) })
        }

        val surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@EmulationActivity)
        }
        statusView = TextView(this).apply {
            textSize = 14f
            setTextColor(0xffffffff.toInt())
            setBackgroundColor(0x44000000)
            setPadding(24, 24, 24, 24)
            text = "NxEmu EmulationActivity\nwaiting for Surface...\npath=$gamePath"
        }
        inputStateView = TextView(this).apply {
            textSize = 11f
            setTextColor(0xffffff00.toInt())
            setBackgroundColor(0x00000000)
            alpha = 0f
            setPadding(0, 0, 0, 0)
            text = "input: 等待触摸按键"
            visibility = android.view.View.GONE
        }
        perfView = TextView(this).apply {
            textSize = 14f
            setTextColor(0xff00ff66.toInt())
            setBackgroundColor(0x66000000)
            setPadding(18, 10, 18, 10)
            text = "FPS -- | Speed -- | Temp -- | Skip $frameSkip | Res ${resolutionLabel()} | ${aspectRatioLabel()} | ${nceStatusLabel()}"
        }
        frameSkipButton = Button(this).apply {
            text = "跳帧 $frameSkip"
            applyControlButtonStyle()
            setOnClickListener {
                frameSkip = (frameSkip + 1) % 5
                applyPerformanceProfile("frameskip-button")
            }
        }
        resolutionButton = Button(this).apply {
            text = "分辨率 ${resolutionLabel()}"
            applyControlButtonStyle()
            setOnClickListener {
                resolutionSetup = when (resolutionSetup) {
                    1 -> 0   // 3/4X -> 1/2X
                    0 -> 2   // 1/2X -> 1X
                    else -> 1
                }
                applyPerformanceProfile("resolution-button")
            }
        }
        aspectRatioButton = Button(this).apply {
            text = "画面 ${aspectRatioLabel()}"
            applyControlButtonStyle()
            setOnClickListener {
                // 和 yuzu/Eden/Citron 的 renderer aspect ratio 思路保持一致：
                // 默认 Stretch to window；需要排查画面比例/黑边时可切回 16:9。
                aspectRatio = when (aspectRatio) {
                    4 -> 0       // Stretch -> 16:9
                    0 -> 2       // 16:9 -> 21:9
                    2 -> 3       // 21:9 -> 16:10
                    3 -> 1       // 16:10 -> 4:3
                    else -> 4    // 4:3/未知 -> Stretch
                }
                applyPerformanceProfile("aspect-ratio-button")
            }
        }
        nceButton = Button(this).apply {
            text = nceButtonLabel()
            applyControlButtonStyle()
            setOnClickListener {
                preferNce = !preferNce
                applyPerformanceProfile("nce-button")
            }
        }
        perfHudButton = Button(this).apply {
            text = perfHudButtonLabel()
            applyControlButtonStyle()
            setOnClickListener {
                perfHudDetailed = !perfHudDetailed
                AppPreferences.savePerfHudDetailed(this@EmulationActivity, perfHudDetailed)
                text = perfHudButtonLabel()
                val stats = runCatching { NativeLibrary.getPerformanceStats() }.getOrElse { "" }
                updatePerformanceOverlay(stats)
                appendSessionLog(
                    "perf-hud-toggle",
                    "detailed=$perfHudDetailed\nperformance:\n$stats",
                    includeSnapshot = false
                )
            }
        }
        graphicsCompatButton = Button(this).apply {
            text = graphicsCompatButtonLabel()
            applyControlButtonStyle()
            setOnClickListener {
                graphicsCompat = !graphicsCompat
                applyPerformanceProfile("graphics-compat-button")
                Toast.makeText(
                    this@EmulationActivity,
                    if (graphicsCompat) "图形兼容模式已开：建议重启游戏复测黑块/花屏" else "图形兼容模式已关：恢复性能优先",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
        stopButton = Button(this).apply {
            text = "停止运行并返回"
            applyControlButtonStyle()
            setOnClickListener { stopEmulationAndFinish() }
        }
        val plusButton = createInputButton("+", SWITCH_BUTTON_PLUS)
        toggleOverlayButton = Button(this).apply {
            text = "隐藏诊断"
            applyControlButtonStyle()
            setOnClickListener {
                val hide = overlayScroll.visibility == android.view.View.VISIBLE
                overlayScroll.visibility = if (hide) android.view.View.GONE else android.view.View.VISIBLE
                text = if (hide) "显示诊断" else "隐藏诊断"
            }
        }
        val copyLogButton = Button(this).apply {
            text = "复制日志"
            applyControlButtonStyle()
            setOnClickListener { copyDiagnosticsToClipboard() }
        }
        val saveLogButton = Button(this).apply {
            text = "保存日志"
            applyControlButtonStyle()
            setOnClickListener {
                val path = saveDiagnosticsToNsLogs("manual-button")
                Toast.makeText(
                    this@EmulationActivity,
                    if (path.isNotBlank()) "日志已保存: $path" else "日志保存失败",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
        val sampleButton = Button(this).apply {
            text = "采样"
            applyControlButtonStyle()
            setOnClickListener {
                val result = runCatching { NativeLibrary.requestGuestCpuSample() }
                    .getOrElse { "cpuSample=failed\n${it::class.java.simpleName}:${it.message}" }
                inputStateView.text = "CPU采样: ${result.lineSequence().firstOrNull().orEmpty()}"
                statusView.append("\nmanual cpu sample:\n$result")
            }
        }
        val nextLoadButton = Button(this).apply {
            text = "启动Next"
            applyControlButtonStyle()
            setOnClickListener { launchPendingNextLoad("manual") }
        }
        val systemBarsButton = Button(this).apply {
            text = "系统栏"
            applyControlButtonStyle()
            setOnClickListener { showSystemBarsTemporarily() }
        }
        val minimizeButton = Button(this).apply {
            text = "最小化"
            applyControlButtonStyle()
            setOnClickListener {
                appendSessionLog("minimize-button", "moveTaskToBack=true", includeSnapshot = true)
                moveTaskToBack(true)
            }
        }
        touchToggleButton = Button(this).apply {
            text = "隐藏触控"
            applyControlButtonStyle()
            setOnClickListener {
                setTouchControlsVisible(!touchControlsVisible, "toolbar-button")
                Toast.makeText(
                    this@EmulationActivity,
                    if (touchControlsVisible) "触控按键已显示" else "触控按键已隐藏；点左上角“设置”可重新显示",
                    Toast.LENGTH_SHORT
                ).show()
            }
        }

        overlayScroll = ScrollView(this).apply { addView(statusView) }

        val minusButton = createInputButton("-", SWITCH_BUTTON_MINUS)
        val selectButton = createInputButton("Select", SWITCH_BUTTON_MINUS)
        val startButton = createInputButton("Start", SWITCH_BUTTON_PLUS)
        val dpadLeftButton = createInputButton("←", SWITCH_BUTTON_LEFT)
        val dpadUpButton = createInputButton("↑", SWITCH_BUTTON_UP)
        val dpadDownButton = createInputButton("↓", SWITCH_BUTTON_DOWN)
        val dpadRightButton = createInputButton("→", SWITCH_BUTTON_RIGHT)
        val buttonY = createInputButton("Y", SWITCH_BUTTON_Y)
        val buttonX = createInputButton("X", SWITCH_BUTTON_X)
        val buttonB = createInputButton("B", SWITCH_BUTTON_B)
        val buttonA = createInputButton("A", SWITCH_BUTTON_A)
        val shoulderLButton = createInputButton("L", SWITCH_BUTTON_L)
        val shoulderRButton = createInputButton("R", SWITCH_BUTTON_R)
        val triggerLButton = createInputButton("LT", SWITCH_BUTTON_ZL)
        val triggerRButton = createInputButton("RT", SWITCH_BUTTON_ZR)
        val l3Button = createInputButton("L3", SWITCH_BUTTON_LSTICK)
        val r3Button = createInputButton("R3", SWITCH_BUTTON_RSTICK)
        val leftStickView = createAnalogStickView("L", SWITCH_ANALOG_LEFT)
        val rightStickView = createAnalogStickView("R", SWITCH_ANALOG_RIGHT)

        fun drawerHeader(textValue: String): TextView = TextView(this).apply {
            text = textValue
            textSize = 18f
            setTextColor(0xffffffff.toInt())
            setBackgroundColor(0xff202020.toInt())
            setPadding(22, 18, 22, 18)
        }
        fun drawerButtonParams(): LinearLayout.LayoutParams =
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, SETTINGS_DRAWER_BUTTON_HEIGHT).apply {
                setMargins(10, 6, 10, 6)
            }

        val quickSettingsPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            alpha = DRAWER_ALPHA
            setBackgroundColor(DRAWER_BG_COLOR)
            setPadding(6, 6, 6, 12)
            addView(drawerHeader("Quick Settings"))
            listOf(
                frameSkipButton,
                resolutionButton,
                aspectRatioButton,
                graphicsCompatButton,
                nceButton,
                perfHudButton,
                touchToggleButton
            ).forEach { button ->
                button.layoutParams = drawerButtonParams()
                addView(button)
            }
            visibility = View.GONE
        }

        // Eden uses DrawerLayout with an in-game menu on the left and quick settings on the right.
        // Keep this project dependency-free for now, but mirror the interaction model with two
        // native side panels instead of the old top-wide toolbar that covered too much gameplay.
        val toolBar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            alpha = DRAWER_ALPHA
            setBackgroundColor(DRAWER_BG_COLOR)
            setPadding(6, 6, 6, 12)
            addView(drawerHeader("NxEmu Menu"))
            listOf(
                stopButton,
                minimizeButton,
                systemBarsButton,
                toggleOverlayButton,
                copyLogButton,
                saveLogButton,
                sampleButton,
                nextLoadButton
            ).forEach { button ->
                button.layoutParams = drawerButtonParams()
                addView(button)
            }
            visibility = View.GONE
        }
        val settingsButton = Button(this).apply {
            text = "☰"
            applyControlButtonStyle()
            textSize = 20f
            alpha = EDEN_HANDLE_ALPHA
            setOnClickListener {
                val show = toolBar.visibility != View.VISIBLE
                toolBar.visibility = if (show) View.VISIBLE else View.GONE
                quickSettingsPanel.visibility = View.GONE
                text = if (show) "×" else "☰"
                hideSystemBars()
                if (show) {
                    mainHandler.postDelayed({
                        toolBar.visibility = View.GONE
                        text = "☰"
                    }, DRAWER_AUTO_HIDE_MS)
                }
            }
        }
        val quickSettingsButton = Button(this).apply {
            text = "⚙"
            applyControlButtonStyle()
            textSize = 20f
            alpha = EDEN_HANDLE_ALPHA
            setOnClickListener {
                val show = quickSettingsPanel.visibility != View.VISIBLE
                quickSettingsPanel.visibility = if (show) View.VISIBLE else View.GONE
                toolBar.visibility = View.GONE
                settingsButton.text = "☰"
                text = if (show) "×" else "⚙"
                hideSystemBars()
                if (show) {
                    mainHandler.postDelayed({
                        quickSettingsPanel.visibility = View.GONE
                        text = "⚙"
                    }, DRAWER_AUTO_HIDE_MS)
                }
            }
        }

        lateinit var pauseMenuPanel: LinearLayout
        fun pauseMenuButton(label: String, primary: Boolean = false, action: () -> Unit): Button = Button(this).apply {
            text = label
            applyControlButtonStyle()
            textSize = if (primary) 17f else 14f
            alpha = if (primary) 0.96f else 0.88f
            setBackgroundColor(if (primary) 0xee5b5ff0.toInt() else 0xdd252a36.toInt())
            setOnClickListener { action() }
            layoutParams = LinearLayout.LayoutParams(0, 74, 1f).apply { setMargins(8, 8, 8, 8) }
        }
        val pauseExitButton = Button(this).apply {
            text = "退出运行"
            applyControlButtonStyle()
            textSize = 14f
            alpha = 0.90f
            setBackgroundColor(0xdd6b2028.toInt())
            layoutParams = LinearLayout.LayoutParams(0, 74, 1f).apply { setMargins(8, 8, 8, 8) }
            setOnClickListener {
                val now = System.currentTimeMillis()
                if (now - lastBackPressMs < BACK_EXIT_CONFIRM_MS) {
                    appendSessionLog("pause-menu-exit", "confirmed=true", includeSnapshot = true)
                    stopEmulationAndFinish()
                } else {
                    lastBackPressMs = now
                    text = "再次点击确认退出"
                    Toast.makeText(this@EmulationActivity, "再次点击确认退出模拟器", Toast.LENGTH_SHORT).show()
                    appendSessionLog("pause-menu-exit", "confirmed=false", includeSnapshot = false)
                    mainHandler.postDelayed({ text = "退出运行" }, BACK_EXIT_CONFIRM_MS + 300L)
                }
            }
        }
        pauseMenuPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            alpha = 0.94f
            setBackgroundColor(0xee101014.toInt())
            setPadding(18, 16, 18, 18)
            visibility = View.GONE
            addView(TextView(this@EmulationActivity).apply {
                text = "NxEmu Pause Menu"
                textSize = 24f
                setTextColor(0xffffffff.toInt())
                setPadding(12, 10, 12, 4)
            })
            addView(TextView(this@EmulationActivity).apply {
                text = buildString {
                    append(gameName.ifBlank { gamePath.substringAfterLast('/') }.ifBlank { "Running game" })
                    append("\n")
                    append("FPS/HUD、触控、日志和退出都可以在这里操作")
                }
                textSize = 12f
                setTextColor(0xffaeb8dc.toInt())
                setPadding(12, 0, 12, 10)
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(pauseMenuButton("继续", primary = true) {
                    pauseMenuPanel.visibility = View.GONE
                    hideSystemBars()
                })
                addView(pauseMenuButton(if (touchControlsVisible) "隐藏触控" else "显示触控") {
                    setTouchControlsVisible(!touchControlsVisible, "pause-menu")
                    pauseMenuPanel.visibility = View.GONE
                })
                addView(pauseMenuButton("HUD ${if (perfHudDetailed) "详细" else "精简"}") {
                    perfHudButton.performClick()
                    pauseMenuPanel.visibility = View.GONE
                })
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(pauseMenuButton("复制日志") { copyDiagnosticsToClipboard() })
                addView(pauseMenuButton("保存日志") {
                    val path = saveDiagnosticsToNsLogs("pause-menu")
                    Toast.makeText(this@EmulationActivity, if (path.isNotBlank()) "日志已保存: $path" else "日志保存失败", Toast.LENGTH_LONG).show()
                })
                addView(pauseMenuButton("系统栏") { showSystemBarsTemporarily() })
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(pauseMenuButton("最小化") {
                    appendSessionLog("pause-menu-minimize", "moveTaskToBack=true", includeSnapshot = true)
                    moveTaskToBack(true)
                })
                addView(pauseExitButton)
                addView(pauseMenuButton("关闭菜单") {
                    pauseMenuPanel.visibility = View.GONE
                    hideSystemBars()
                })
            })
        }
        val pauseMenuButtonHandle = Button(this).apply {
            text = "Ⅱ"
            applyControlButtonStyle()
            textSize = 20f
            alpha = EDEN_HANDLE_ALPHA
            setOnClickListener {
                val show = pauseMenuPanel.visibility != View.VISIBLE
                pauseMenuPanel.visibility = if (show) View.VISIBLE else View.GONE
                toolBar.visibility = View.GONE
                quickSettingsPanel.visibility = View.GONE
                settingsButton.text = "☰"
                quickSettingsButton.text = "⚙"
                hideSystemBars()
            }
        }

        val dpadPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            alpha = touchButtonAlpha()
            setPadding(8, 8, 8, 8)
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addSpacer(DPAD_BUTTON_SIZE)
                addTouchButton(dpadUpButton, DPAD_BUTTON_SIZE, DPAD_BUTTON_SIZE)
                addSpacer(DPAD_BUTTON_SIZE)
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addTouchButton(dpadLeftButton, DPAD_BUTTON_SIZE, DPAD_BUTTON_SIZE)
                addSpacer(DPAD_BUTTON_SIZE)
                addTouchButton(dpadRightButton, DPAD_BUTTON_SIZE, DPAD_BUTTON_SIZE)
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addSpacer(DPAD_BUTTON_SIZE)
                addTouchButton(dpadDownButton, DPAD_BUTTON_SIZE, DPAD_BUTTON_SIZE)
                addSpacer(DPAD_BUTTON_SIZE)
            })
        }

        val leftStickPanel = FrameLayout(this).apply {
            alpha = stickAlpha()
            addView(
                leftStickView,
                FrameLayout.LayoutParams(scaledControlSize(ANALOG_STICK_SIZE), scaledControlSize(ANALOG_STICK_SIZE))
            )
        }

        val rightStickPanel = FrameLayout(this).apply {
            alpha = stickAlpha()
            addView(
                rightStickView,
                FrameLayout.LayoutParams(scaledControlSize(ANALOG_STICK_SIZE), scaledControlSize(ANALOG_STICK_SIZE))
            )
        }

        val facePanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            alpha = touchButtonAlpha()
            setPadding(8, 8, 8, 8)
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addSpacer(FACE_BUTTON_SIZE)
                addTouchButton(buttonX, FACE_BUTTON_SIZE, FACE_BUTTON_SIZE)
                addSpacer(FACE_BUTTON_SIZE)
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addTouchButton(buttonY, FACE_BUTTON_SIZE, FACE_BUTTON_SIZE)
                addSpacer(FACE_BUTTON_SIZE)
                addTouchButton(buttonA, FACE_BUTTON_SIZE, FACE_BUTTON_SIZE)
            })
            addView(LinearLayout(this@EmulationActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                addSpacer(FACE_BUTTON_SIZE)
                addTouchButton(buttonB, FACE_BUTTON_SIZE, FACE_BUTTON_SIZE)
                addSpacer(FACE_BUTTON_SIZE)
            })
        }

        val leftShoulderPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = touchButtonAlpha()
            setPadding(4, 4, 4, 4)
            addTouchButton(shoulderLButton, SHOULDER_BUTTON_WIDTH, SHOULDER_BUTTON_HEIGHT)
            addTouchButton(triggerLButton, SHOULDER_BUTTON_WIDTH, SHOULDER_BUTTON_HEIGHT)
        }

        val rightShoulderPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = touchButtonAlpha()
            setPadding(4, 4, 4, 4)
            addTouchButton(triggerRButton, SHOULDER_BUTTON_WIDTH, SHOULDER_BUTTON_HEIGHT)
            addTouchButton(shoulderRButton, SHOULDER_BUTTON_WIDTH, SHOULDER_BUTTON_HEIGHT)
        }

        val leftStickClickPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = touchButtonAlpha()
            setPadding(4, 4, 4, 4)
            addTouchButton(l3Button, STICK_CLICK_BUTTON_SIZE, STICK_CLICK_BUTTON_SIZE)
        }

        val rightStickClickPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = touchButtonAlpha()
            setPadding(4, 4, 4, 4)
            addTouchButton(r3Button, STICK_CLICK_BUTTON_SIZE, STICK_CLICK_BUTTON_SIZE)
        }

        val centerPanel = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            alpha = touchButtonAlpha()
            setPadding(4, 4, 4, 4)
            addTouchButton(selectButton, SMALL_BUTTON_WIDTH, SMALL_BUTTON_HEIGHT)
            addSpacer(CENTER_BUTTON_GAP)
            addTouchButton(minusButton, SMALL_BUTTON_HEIGHT, SMALL_BUTTON_HEIGHT)
            addSpacer(CENTER_BUTTON_GAP)
            addTouchButton(startButton, SMALL_BUTTON_WIDTH, SMALL_BUTTON_HEIGHT)
            addSpacer(CENTER_BUTTON_GAP)
            addTouchButton(plusButton, SMALL_BUTTON_HEIGHT, SMALL_BUTTON_HEIGHT)
        }
        touchControlViews.clear()
        touchControlViews.addAll(
            listOf(
                leftShoulderPanel,
                rightShoulderPanel,
                leftStickPanel,
                leftStickClickPanel,
                dpadPanel,
                rightStickPanel,
                rightStickClickPanel,
                facePanel,
                centerPanel
            )
        )

        setContentView(
            FrameLayout(this).apply {
                addView(surfaceView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
                addView(
                    overlayScroll,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT
                    )
                )
                addView(
                    perfView,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.TOP or Gravity.CENTER_HORIZONTAL
                    ).apply { topMargin = 10 }
                )
                addView(
                    toolBar,
                    FrameLayout.LayoutParams(
                        SETTINGS_DRAWER_WIDTH,
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        Gravity.LEFT
                    )
                )
                addView(
                    quickSettingsPanel,
                    FrameLayout.LayoutParams(
                        SETTINGS_DRAWER_WIDTH,
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        Gravity.RIGHT
                    )
                )
                addView(
                    settingsButton,
                    FrameLayout.LayoutParams(
                        EDEN_HANDLE_SIZE,
                        EDEN_HANDLE_SIZE,
                        Gravity.LEFT or Gravity.TOP
                    ).apply {
                        leftMargin = 14
                        topMargin = 14
                    }
                )
                addView(
                    quickSettingsButton,
                    FrameLayout.LayoutParams(
                        EDEN_HANDLE_SIZE,
                        EDEN_HANDLE_SIZE,
                        Gravity.RIGHT or Gravity.TOP
                    ).apply {
                        rightMargin = 14
                        topMargin = 14
                    }
                )
                addView(
                    pauseMenuButtonHandle,
                    FrameLayout.LayoutParams(
                        EDEN_HANDLE_SIZE,
                        EDEN_HANDLE_SIZE,
                        Gravity.TOP or Gravity.CENTER_HORIZONTAL
                    ).apply { topMargin = 72 }
                )
                addView(
                    pauseMenuPanel,
                    FrameLayout.LayoutParams(
                        PAUSE_MENU_WIDTH,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.CENTER
                    )
                )
                addView(
                    leftShoulderPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.LEFT or Gravity.TOP
                    ).apply {
                        leftMargin = 24
                        topMargin = 92
                    }
                )
                addView(
                    rightShoulderPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.RIGHT or Gravity.TOP
                    ).apply {
                        rightMargin = 24
                        topMargin = 92
                    }
                )
                addView(
                    leftStickPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.LEFT or Gravity.BOTTOM
                    ).apply {
                        leftMargin = 34
                        bottomMargin = 44
                    }
                )
                addView(
                    leftStickClickPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.LEFT or Gravity.BOTTOM
                    ).apply {
                        leftMargin = 500
                        bottomMargin = 138
                    }
                )
                addView(
                    dpadPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.LEFT or Gravity.BOTTOM
                    ).apply {
                        leftMargin = 44
                        bottomMargin = 520
                    }
                )
                addView(
                    rightStickPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.RIGHT or Gravity.BOTTOM
                    ).apply {
                        rightMargin = 34
                        bottomMargin = 44
                    }
                )
                addView(
                    rightStickClickPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.RIGHT or Gravity.BOTTOM
                    ).apply {
                        rightMargin = 500
                        bottomMargin = 138
                    }
                )
                addView(
                    facePanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.RIGHT or Gravity.BOTTOM
                    ).apply {
                        rightMargin = 44
                        bottomMargin = 550
                    }
                )
                addView(
                    centerPanel,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.CENTER_HORIZONTAL or Gravity.BOTTOM
                    ).apply { bottomMargin = 84 }
                )
            }
        )
        hideSystemBars()
        setTouchControlsVisible(touchControlsVisible, "activity-create", writeLog = false)
        perfView.text = "FPS -- | Speed -- | Temp --"
        Log.i("NxEmuAndroid", "starting performance samplers")
        mainHandler.post(perfTicker)
        // 只保留 UI 主线程 perfTicker。旧版额外启动 NxEmuPerfSampler 后台 Java 线程，
        // 在 NCE 运行时会和进程级 SIGSEGV handler/ART JIT signal chain 相撞：
        // 用户三连测日志中可见 NxEmuPerfSample 线程在 libart JitCodeCache::LookupMethodHeader
        // 触发 SEGV_ACCERR，随后被 NCE host fault 路径转成进程崩溃。
        // FPS/卡顿诊断改由 perfTicker 采集，避免多一个 Java/JIT 线程干扰 NCE。
        applyPerformanceProfile("activity-create")
        appendSessionLog("activity-create-ui-ready", "ui=ready\nsettings: frameSkip=$frameSkip resolution=${resolutionLabel()} aspect=${aspectRatioLabel()}($aspectRatio) graphicsCompat=$graphicsCompat preferNce=$preferNce effectivePreferNce=${effectivePreferNce()} androidNceGuard=$ANDROID_NCE_STABILITY_GUARD")
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val controller = window.decorView.windowInsetsController
            if (controller != null) {
                controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                controller.hide(WindowInsets.Type.systemBars())
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )
        }
    }

    private fun showSystemBarsTemporarily() {
        mainHandler.removeCallbacks(hideSystemBarsRunnable)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.windowInsetsController?.show(WindowInsets.Type.systemBars())
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                )
        }
        mainHandler.postDelayed(hideSystemBarsRunnable, 3500)
    }

    private fun touchButtonAlpha(): Float = (TOUCH_ALPHA * touchOpacityScale).coerceIn(0.20f, 1.0f)

    private fun stickAlpha(): Float = (STICK_ALPHA * touchOpacityScale).coerceIn(0.20f, 1.0f)

    private fun scaledControlSize(base: Int): Int {
        val scale = when (touchLayoutPreset.coerceIn(0, 2)) {
            0 -> 0.84f
            2 -> 1.16f
            else -> 1.0f
        }
        return (base * scale).toInt().coerceAtLeast(48)
    }

    private fun setTouchControlsVisible(
        visible: Boolean,
        reason: String,
        writeLog: Boolean = true
    ) {
        touchControlsVisible = visible
        touchControlViews.forEach { view ->
            view.visibility = if (visible) View.VISIBLE else View.GONE
        }
        if (::touchToggleButton.isInitialized) {
            touchToggleButton.text = if (visible) "隐藏触控" else "显示触控"
        }
        if (writeLog) {
            appendSessionLog(
                "touch-controls-$reason",
                "visible=$visible views=${touchControlViews.size}",
                includeSnapshot = false
            )
        }
        hideSystemBars()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        appendSessionLog("surface-created", "surface=${holder.surface}")
        attachSurfaceAndMaybeBoot(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        appendSessionLog("surface-changed", "format=$format width=$width height=$height surface=${holder.surface}")
        attachSurfaceAndMaybeBoot(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceAlive = false
        // Surface 销毁后 Vulkan present 线程继续使用旧 Surface 会崩。
        // 这里优先请求 shutdown，退出 Activity 时再由 onDestroy 做最终清理。
        val result = if (bootStarted && !stopRequested.get()) {
            stopRequested.set(true)
            runCatching { NativeLibrary.shutdownRuntime() }
                .getOrElse { "shutdown=exception\n${it.stackTraceToString()}" }
        } else {
            "surface=destroyed-java-only\nnativeSurface=kept-until-shutdown"
        }
        statusView.append(
            buildString {
                appendLine()
                appendLine("surfaceDestroyed:")
                appendLine(result)
                appendLine("surface=destroyed")
            }
        )
        appendSessionLog("surface-destroyed", "setSurface:\n$result", includeSnapshot = true)
        saveDiagnosticsToNsLogs("surface-destroyed")
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(perfTicker)
        mainHandler.removeCallbacks(hideSystemBarsRunnable)
        runCatching { unregisterReceiver(debugInputReceiver) }
        appendSessionLog("activity-destroy-before-shutdown", includeSnapshot = true)
        saveDiagnosticsToNsLogs("activity-destroy-before-shutdown")
        val shutdownResult = runCatching {
            stopNativeRuntime()
        }.getOrElse { "shutdown=exception\n${it.stackTraceToString()}" }
        appendSessionLog("activity-destroy-shutdown-result", shutdownResult, includeSnapshot = false)
        saveDiagnosticsToNsLogs("activity-destroy-after-shutdown")
        appendSessionLog("activity-destroy-after-shutdown", includeSnapshot = true)
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        appendSessionLog("activity-resume", "surfaceAlive=$surfaceAlive bootStarted=$bootStarted stopRequested=${stopRequested.get()}")
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onPause() {
        appendSessionLog("activity-pause", "surfaceAlive=$surfaceAlive bootStarted=$bootStarted stopRequested=${stopRequested.get()}")
        super.onPause()
    }

    override fun onStop() {
        appendSessionLog("activity-stop", "surfaceAlive=$surfaceAlive bootStarted=$bootStarted stopRequested=${stopRequested.get()}")
        super.onStop()
    }

    override fun onBackPressed() {
        val now = System.currentTimeMillis()
        if (now - lastBackPressMs < BACK_EXIT_CONFIRM_MS) {
            appendSessionLog("back-confirm-exit", "confirmed=true", includeSnapshot = true)
            stopEmulationAndFinish()
        } else {
            lastBackPressMs = now
            Toast.makeText(this, "再按一次返回键退出模拟器", Toast.LENGTH_SHORT).show()
            appendSessionLog("back-confirm-exit", "confirmed=false", includeSnapshot = false)
            hideSystemBars()
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            onBackPressed()
            return true
        }
        val ordinal = mapGamepadKey(keyCode) ?: return super.onKeyDown(keyCode, event)
        if (event.repeatCount == 0) {
            appendInputResult("pad-${KeyEvent.keyCodeToString(keyCode)}", ordinal, true)
        }
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val ordinal = mapGamepadKey(keyCode) ?: return super.onKeyUp(keyCode, event)
        appendInputResult("pad-${KeyEvent.keyCodeToString(keyCode)}", ordinal, false)
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if ((event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
            (event.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        ) {
            val lx = getCenteredAxis(event, MotionEvent.AXIS_X)
            val ly = getCenteredAxis(event, MotionEvent.AXIS_Y)
            val rx = when {
                kotlin.math.abs(getCenteredAxis(event, MotionEvent.AXIS_Z)) > 0f -> getCenteredAxis(event, MotionEvent.AXIS_Z)
                else -> getCenteredAxis(event, MotionEvent.AXIS_RX)
            }
            val ry = when {
                kotlin.math.abs(getCenteredAxis(event, MotionEvent.AXIS_RZ)) > 0f -> getCenteredAxis(event, MotionEvent.AXIS_RZ)
                else -> getCenteredAxis(event, MotionEvent.AXIS_RY)
            }
            appendAnalogResult("pad-L", SWITCH_ANALOG_LEFT, lx, ly)
            appendAnalogResult("pad-R", SWITCH_ANALOG_RIGHT, rx, ry)
            return true
        }
        return super.onGenericMotionEvent(event)
    }

    private fun getCenteredAxis(event: MotionEvent, axis: Int): Float {
        val range = event.device?.getMotionRange(axis, event.source)
        val flat = range?.flat ?: ANALOG_DEADZONE
        val value = event.getAxisValue(axis)
        return if (kotlin.math.abs(value) > flat) value.coerceIn(-1f, 1f) else 0f
    }

    private fun mapGamepadKey(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> SWITCH_BUTTON_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> SWITCH_BUTTON_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> SWITCH_BUTTON_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> SWITCH_BUTTON_RIGHT
        // Android gamepad physical A/B are opposite to Switch layout on many controllers.
        // Swap them for guest-visible Switch A/B; touch overlay keeps its original mapping.
        KeyEvent.KEYCODE_BUTTON_A -> SWITCH_BUTTON_B
        KeyEvent.KEYCODE_BUTTON_B -> SWITCH_BUTTON_A
        KeyEvent.KEYCODE_BUTTON_X -> SWITCH_BUTTON_X
        KeyEvent.KEYCODE_BUTTON_Y -> SWITCH_BUTTON_Y
        KeyEvent.KEYCODE_BUTTON_L1 -> SWITCH_BUTTON_L
        KeyEvent.KEYCODE_BUTTON_R1 -> SWITCH_BUTTON_R
        KeyEvent.KEYCODE_BUTTON_L2 -> SWITCH_BUTTON_ZL
        KeyEvent.KEYCODE_BUTTON_R2 -> SWITCH_BUTTON_ZR
        KeyEvent.KEYCODE_BUTTON_THUMBL -> SWITCH_BUTTON_LSTICK
        KeyEvent.KEYCODE_BUTTON_THUMBR -> SWITCH_BUTTON_RSTICK
        KeyEvent.KEYCODE_BUTTON_START -> SWITCH_BUTTON_PLUS
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_MODE -> SWITCH_BUTTON_MINUS
        else -> null
    }

    private fun attachSurfaceAndMaybeBoot(surface: Surface) {
        surfaceAlive = true
        Log.i("NxEmuAndroid", "surface callback bootStarted=$bootStarted valid=${surface.isValid}")
        appendSessionLog(
            "surface-callback",
            "bootStarted=$bootStarted surfaceAlive=$surfaceAlive valid=${surface.isValid} surface=$surface"
        )
        if (bootStarted) {
            val result = NativeLibrary.setSurface(surface)
            statusView.append("\n\nsurfaceChanged:\n$result")
            appendSessionLog("surface-reattach", result, includeSnapshot = true)
            return
        }
        appendSessionLog("boot-schedule", "surfaceAlive=$surfaceAlive gamePath=$gamePath")

        statusView.text = buildString {
            appendLine("NxEmu EmulationActivity")
            appendLine("path=$gamePath")
            appendLine("exists=${NativeLibrary.exists(gamePath)}")
            appendLine("size=${NativeLibrary.getSize(gamePath)}")
            appendLine("bootMode=background single-shot")
            appendLine()
            appendLine("准备初始化 native runtime / Surface / LoadRom ...")
        }

        mainHandler.postDelayed({
            if (!bootStarted && surfaceAlive && surface.isValid && !stopRequested.get()) {
                startBootThread(surface)
            } else {
                appendSessionLog(
                    "boot-delay-skipped",
                    "bootStarted=$bootStarted surfaceAlive=$surfaceAlive valid=${surface.isValid} stopRequested=${stopRequested.get()}"
                )
            }
        }, BOOT_SURFACE_SETTLE_DELAY_MS)
    }

    private fun startBootThread(surface: Surface) {
        if (bootStarted) return
        bootStarted = true
        appendSessionLog(
            "boot-thread-schedule",
            "surfaceAlive=$surfaceAlive valid=${surface.isValid} gamePath=$gamePath"
        )
        bootThread = Thread({
            appendSessionLog("boot-thread-begin", "thread=${Thread.currentThread().name}")
            val result = runCatching {
                buildString {
                    appendLine("NxEmu EmulationActivity")
                    appendLine("path=$gamePath")
                    appendLine("exists=${NativeLibrary.exists(gamePath)}")
                    appendLine("size=${NativeLibrary.getSize(gamePath)}")
                    appendLine("bootMode=background single-shot")
                    appendLine("displayMode=diagnostic overlay")
                    appendLine("settings: frameSkip=$frameSkip resolution=${resolutionLabel()} aspect=${aspectRatioLabel()} graphicsCompat=$graphicsCompat preferNce=$preferNce")
                    appendLine()
                    appendLine("画面说明:")
                    appendLine("- 本页文字是 Android 诊断 overlay；底层图形来自 native Surface。")
                    appendLine("- DXCI/DNSP：boot=LoadRom accepted 表示容器/loader 已接收，后续还要看 guest/render/service 日志。")
                    appendLine("- NRO/hbmenu：看到 hbmenu 图形界面表示 NRO/HBL env/romfs/assets/render 已开始工作。")
                    appendLine("- 如果只有 overlay 或黑屏，点“复制日志”发回来继续补具体阶段。")
                    appendLine()
                    appendLine("initialize:")
                    val initializeResult = NativeLibrary.initialize(NativeLibrary::class.java)
                    appendLine(initializeResult)
                    appendSessionLog("native-initialize", initializeResult.toString())
                    appendLine()
                    appendLine("gpuDriver:")
                    if (gpuDriverSource.isNotBlank()) {
                        val driverSelectResult = GpuDriverHelper.useDriverSource(gpuDriverSource)
                        appendLine("perGameDriver:")
                        appendLine(driverSelectResult)
                        appendSessionLog("per-game-driver", driverSelectResult)
                    }
                    val gpuResult = GpuDriverHelper.initialize(this@EmulationActivity)
                    appendLine(gpuResult)
                    appendSessionLog("gpu-driver-initialize", gpuResult)
                    appendLine()
                    appendLine("runtime:")
                    val runtimeResult = NativeLibrary.initializeRuntime(filesDir.absolutePath, applicationInfo.nativeLibraryDir)
                    appendLine(runtimeResult)
                    appendSessionLog("runtime-initialize", runtimeResult)
                    appendLine()
                    appendLine("surface:")
                    val surfaceResult = if (surfaceAlive && surface.isValid) {
                        NativeLibrary.setSurface(surface)
                    } else {
                        "surface=destroyed-before-attach\nvalid=${surface.isValid}"
                    }
                    appendLine(surfaceResult)
                    appendSessionLog("surface-attach", surfaceResult)
                    appendLine()
                    appendLine("boot:")
                    val bootResult = if (surfaceAlive && surface.isValid) {
                        appendSessionLog("boot-game-begin", "gamePath=$gamePath\nexists=${NativeLibrary.exists(gamePath)}\nsize=${NativeLibrary.getSize(gamePath)}")
                        NativeLibrary.bootGame(gamePath)
                    } else {
                        "boot=failed\nreason=surface destroyed before bootGame\nvalid=${surface.isValid}"
                    }
                    appendLine(bootResult)
                    // 这里不要 includeSnapshot：boot 刚返回时 guest/render 线程可能正在初始化，
                    // 同步调用 runtimeStatus/performanceStats 可能和 native 锁竞争，导致自动日志断在 boot-game-begin。
                    appendSessionLog("boot-game-end", bootResult, includeSnapshot = false)
                    appendLine()
                    appendLine("runtime status:")
                    val runtimeStatus = NativeLibrary.runtimeStatus()
                    appendLine(runtimeStatus)
                    appendSessionLog("runtime-status-after-boot", runtimeStatus)
                    appendLine()
                    appendLine("诊断结论:")
                    appendLine("- boot=LoadRom accepted：loader/HLE 已经接收文件。")
                    appendLine("- 对 DXCI/DNSP，下一步看是否创建进程、加载 NSO、进入渲染/服务调用。")
                    appendLine("- 对 NRO/hbmenu，能显示时说明 HBL env、argv0、RomFS/assets 和 Vulkan 基本渲染链路已通过。")
                    appendLine("- 如果 logcat 出现 VK_ERROR/falling back to Null renderer，表示该设备渲染初始化失败。")
                    appendLine("如果显示 LoadRom failed 或 crash，把本页文字/logcat 发回来。")
                }
            }.getOrElse { error ->
                val stack = error.stackTraceToString()
                appendSessionLog("boot-thread-exception", stack, includeSnapshot = true)
                stack
            }

            runOnUiThread {
                if (!stopRequested.get()) {
                    statusView.text = result
                    appendSessionLog("boot-thread-ui-result", result.take(12000), includeSnapshot = false)
                    autoHideDiagnostics()
                }
            }
        }, "NxEmuBootThread").also { it.start() }
        // 默认不再启动自动 CPU 采样。CPU/线程采样会打断 guest cores 并产生大量日志，
        // 在真机上会显著拖慢；需要时点“采样”按钮或复制日志即可。
    }

    private fun startPerformanceSampler() {
        // 保留空壳，避免后续误用；详见 onCreate 中关于 NxEmuPerfSampler/NCE/ART 的说明。
    }

    private fun maybeLogPerfSnapshotAndStall(stats: String) {
        val round = perfTickRound++
        val present = stats.extractDouble("vulkanPresentCount")
        val composite = stats.extractDouble("vulkanCompositeCount")
        if (autoOutputLog && round % PERF_SNAPSHOT_INTERVAL_TICKS == 0) {
            val progress = runCatching { NativeLibrary.getLoadProgress() }
                .getOrElse { "loadProgress=failed\n${it::class.java.simpleName}:${it.message}" }
            appendSessionLog(
                "perf-ticker-snapshot-$round",
                "performance:\n$stats\nloadProgress:\n$progress",
                includeSnapshot = false
            )
        }

        val now = System.currentTimeMillis()
        val hasAnyFrame = present > 0.0 || composite > 0.0
        val advanced = present > lastVulkanPresentCount || composite > lastVulkanCompositeCount
        if (!hasAnyFrame || advanced || !bootStarted || stopRequested.get()) {
            renderStallSinceMs = 0L
            return
        }
        if (renderStallSinceMs == 0L) {
            renderStallSinceMs = now
            return
        }
        val stallMs = now - renderStallSinceMs
        if (stallMs < RENDER_STALL_LOG_AFTER_MS) {
            return
        }
        if (present == lastStallLoggedPresent && composite == lastStallLoggedComposite) {
            return
        }
        lastStallLoggedPresent = present
        lastStallLoggedComposite = composite
        val progress = runCatching { NativeLibrary.getLoadProgress() }
            .getOrElse { "loadProgress=failed\n${it::class.java.simpleName}:${it.message}" }
        // 卡住时立即触发一次 guest CPU 采样。该调用只发中断/设置预算，真正的
        // CPU.Core/OS.Thread 快照会在 guest core 回到调度循环后进入 osServiceDiagnostics。
        val cpuSampleRequest = runCatching { NativeLibrary.requestGuestCpuSample() }
            .getOrElse { "cpuSample=failed\n${it::class.java.simpleName}:${it.message}" }
        val runtime = runCatching { NativeLibrary.runtimeStatus() }
            .getOrElse { "runtimeStatus=failed\n${it::class.java.simpleName}:${it.message}" }
        val text = buildString {
            appendLine("renderStall=detected")
            appendLine("stallMs=$stallMs")
            appendLine("present=${present.format0()}")
            appendLine("composite=${composite.format0()}")
            appendLine("nce=${nceStatusLabel(stats)}")
            appendLine("cpuSampleRequest:")
            appendLine(cpuSampleRequest)
            appendLine("performance:")
            appendLine(stats)
            appendLine("loadProgress:")
            appendLine(progress)
            appendLine("runtimeStatus:")
            appendLine(runtime)
        }
        Log.w("NxEmuAndroid", text)
        appendSessionLog("render-stall-${present.format0()}-${composite.format0()}", text, includeSnapshot = false)
        mainHandler.postDelayed({
            if (!stopRequested.get()) {
                val delayedRuntime = runCatching { NativeLibrary.runtimeStatus() }
                    .getOrElse { "runtimeStatus=failed\n${it::class.java.simpleName}:${it.message}" }
                val delayedStats = runCatching { NativeLibrary.getPerformanceStats() }
                    .getOrElse { "performanceStats=failed\n${it::class.java.simpleName}:${it.message}" }
                appendSessionLog(
                    "render-stall-cpu-sample-${present.format0()}-${composite.format0()}",
                    buildString {
                        appendLine("renderStallCpuSample=delayed")
                        appendLine("present=${present.format0()}")
                        appendLine("composite=${composite.format0()}")
                        appendLine("performance:")
                        appendLine(delayedStats)
                        appendLine("runtimeStatus:")
                        appendLine(delayedRuntime)
                    },
                    includeSnapshot = false
                )
            }
        }, 750L)
    }

    private fun startDiagnosticSampler() {
        if (diagnosticThread != null) return
        diagnosticThread = Thread({
            var round = 0
            while (!stopRequested.get() && round < 120) {
                Thread.sleep(5000)
                val result = runCatching { NativeLibrary.requestGuestCpuSample() }
                    .getOrElse { "cpuSample=failed\n${it::class.java.simpleName}:${it.message}" }
                android.util.Log.i("NxEmuAndroid", "diagnosticSampler round=$round $result")
                round++
            }
        }, "NxEmuDiagSampler").also { it.start() }
    }

    private fun autoHideDiagnostics() {
        overlayScroll.postDelayed({
            if (!stopRequested.get() && overlayScroll.visibility == android.view.View.VISIBLE) {
                overlayScroll.visibility = android.view.View.GONE
                toggleOverlayButton.text = "显示诊断"
            }
        }, 2500)
    }

    private fun applyPerformanceProfile(reason: String) {
        frameSkipButton.text = "跳帧 $frameSkip"
        resolutionButton.text = "分辨率 ${resolutionLabel()}"
        aspectRatioButton.text = "画面 ${aspectRatioLabel()}"
        graphicsCompatButton.text = graphicsCompatButtonLabel()
        nceButton.text = nceButtonLabel()
        AppPreferences.savePerformance(this, frameSkip, resolutionSetup, aspectRatio, graphicsCompat, preferNce)
        val effectiveNce = effectivePreferNce()
        val result = runCatching {
            NativeLibrary.setPerformanceProfile(frameSkip, resolutionSetup, aspectRatio, graphicsCompat, effectiveNce)
        }.getOrElse { error ->
            "setPerformanceProfile=exception\n${error.stackTraceToString()}"
        }
        android.util.Log.i("NxEmuAndroid", "performanceProfile reason=$reason $result")
        inputStateView.text = "性能: skip=$frameSkip res=${resolutionLabel()} aspect=${aspectRatioLabel()} compat=$graphicsCompat ${nceStatusLabel(result)}"
        updatePerformanceOverlay(result)
        appendSessionLog(
            "performance-profile-$reason",
            buildString {
                appendLine("requestedPreferNce=$preferNce")
                appendLine("effectivePreferNce=$effectiveNce")
                appendLine("aspectRatioRequest=$aspectRatio ${aspectRatioLabel()}")
                appendLine("graphicsCompatRequest=$graphicsCompat")
                appendLine("androidNceGuard=$ANDROID_NCE_STABILITY_GUARD")
                append(result)
            }
        )
    }

    private fun updatePerformanceOverlay(nativeStats: String? = null) {
        val stats = nativeStats.orEmpty()
        val hasNceStatus = stats.lineSequence().any { it.startsWith("nceEnabled=") }
        if (hasNceStatus) {
            lastNceActual = stats.extractBoolean("nceEnabled")
        }
        val nowMs = System.currentTimeMillis()
        val presentCount = stats.extractDouble("vulkanPresentCount")
        val compositeCount = stats.extractDouble("vulkanCompositeCount")
        val sampleSeconds = ((nowMs - lastOverlaySampleMs).coerceAtLeast(1)).toDouble() / 1000.0
        if (presentCount >= lastVulkanPresentCount && presentCount > 0.0) {
            lastSafeRenderFps = ((presentCount - lastVulkanPresentCount) / sampleSeconds).coerceAtLeast(0.0)
            if (lastSafeRenderFps > 0.0) {
                lastNonZeroRenderFps = lastSafeRenderFps
                lastFrameCounterMs = nowMs
            }
        }
        if (compositeCount >= lastVulkanCompositeCount && compositeCount > 0.0) {
            lastSafeCompositeFps =
                ((compositeCount - lastVulkanCompositeCount) / sampleSeconds).coerceAtLeast(0.0)
            if (lastSafeCompositeFps > 0.0) {
                lastNonZeroCompositeFps = lastSafeCompositeFps
            }
        }
        if (presentCount > 0.0 || compositeCount > 0.0) {
            lastOverlaySampleMs = nowMs
            lastVulkanPresentCount = presentCount
            lastVulkanCompositeCount = compositeCount
        }
        val speed = stats.extractDouble("speedPercent")
        val nativeGameFps = stats.extractDouble("gameFps")
        val nativeSystemFps = stats.extractDouble("systemFps")
        val nativePresentFps = stats.extractDouble("derivedPresentFps")
        val nativeCompositeFps = stats.extractDouble("derivedCompositeFps")
        val frameAgeMs = nowMs - lastFrameCounterMs
        val displayRenderFps = when {
            lastSafeRenderFps > 0.0 -> lastSafeRenderFps
            lastSafeCompositeFps > 0.0 -> lastSafeCompositeFps
            nativeGameFps > 0.0 -> nativeGameFps
            nativeSystemFps > 0.0 -> nativeSystemFps
            nativePresentFps > 0.0 -> nativePresentFps
            nativeCompositeFps > 0.0 -> nativeCompositeFps
            frameAgeMs < 4500L && lastNonZeroRenderFps > 0.0 -> lastNonZeroRenderFps
            frameAgeMs < 4500L && lastNonZeroCompositeFps > 0.0 -> lastNonZeroCompositeFps
            else -> 0.0
        }
        updateInferredTargetFps(displayRenderFps, stats.extractDouble("targetGameFpsAuto"))
        val fallbackSpeed = if (displayRenderFps > 0.0) {
            (displayRenderFps / inferredTargetFps) * 100.0
        } else {
            0.0
        }
        val nativeTarget = stats.extractDouble("targetGameFpsAuto")
        val displaySpeed = when {
            displayRenderFps > 0.0 -> fallbackSpeed
            nativeTarget > 0.0 && speed > 0.0 -> speed
            speed > 0.0 -> speed
            else -> 0.0
        }
        perfView.text = performanceOverlayText(stats, displayRenderFps, displaySpeed)
    }

    private fun performanceOverlayText(stats: String, displayRenderFps: Double, displaySpeed: Double): String {
        val hasFrameCounter = lastVulkanPresentCount > 0.0 || lastVulkanCompositeCount > 0.0
        val hasAnyFps = displayRenderFps > 0.0
        val fpsText = if (hasAnyFps) displayRenderFps.format1() else "采集中"
        val targetText = if (hasAnyFps && inferredTargetFps > 0.0) {
            "/${inferredTargetFps.formatTargetFps()}"
        } else {
            ""
        }
        val speedText = if (displaySpeed > 0.0) "${displaySpeed.format0()}%" else if (hasFrameCounter) "0%" else "采集中"
        val tempText = readDeviceTemperatureText()
        if (!perfHudDetailed) {
            return "FPS $fpsText$targetText | Speed $speedText | Temp $tempText"
        }

        val backend = stats.extractString("cpuBackendActual").ifBlank { if (lastNceActual) "NCE" else "?" }
        val nce = stats.extractBoolean("nceEnabled")
        val frameMs = stats.extractDouble("frametimeMs")
        val presentFps = stats.extractDouble("derivedPresentFps")
        val compositeFps = stats.extractDouble("derivedCompositeFps")
        val presentCount = stats.extractDouble("vulkanPresentCount")
        val compositeCount = stats.extractDouble("vulkanCompositeCount")
        val counterSymbols = stats.extractString("vulkanCounterSymbols").ifBlank { "?" }
        val fbWidth = stats.extractDouble("vulkanLastFbWidth").toInt()
        val fbHeight = stats.extractDouble("vulkanLastFbHeight").toInt()
        val compat = stats.extractBoolean("graphicsCompat")
        val asyncGpu = stats.extractBoolean("asyncGpu")
        val asyncShader = stats.extractBoolean("asyncShaders")
        val pipelineCache = stats.extractBoolean("vulkanPipelineCache")
        val astc = stats.extractString("astcDecodeMode").ifBlank { "?" }
        val reactive = stats.extractBoolean("reactiveFlushing")
        val gpuAccuracy = stats.extractString("gpuAccuracy").ifBlank { "?" }
        val internalSize = estimatedInternalRenderSize(fbWidth, fbHeight)
        val driver = driverOverlayLabel()
        return buildString {
            appendLine("FPS $fpsText$targetText | Speed $speedText | ${formatMs(frameMs)} | Temp $tempText")
            appendLine("CPU $backend nce=$nce | Skip $frameSkip | Res ${resolutionLabel()} $internalSize | ASTC $astc | ${aspectRatioLabel()}")
            appendLine("GPU $driver | acc=$gpuAccuracy reactive=$reactive asyncG=$asyncGpu asyncS=$asyncShader cache=$pipelineCache compat=$compat")
            append("P ${presentFps.format1()}fps #${presentCount.format0()} | C ${compositeFps.format1()}fps #${compositeCount.format0()} | FB ${fbWidth}x${fbHeight} | ctr $counterSymbols")
        }.trimEnd()
    }

    private fun estimatedInternalRenderSize(fbWidth: Int, fbHeight: Int): String {
        if (fbWidth <= 0 || fbHeight <= 0) {
            return "Internal~--"
        }
        val scale = when (resolutionSetup) {
            0 -> 0.5
            1 -> 0.75
            else -> 1.0
        }
        return "Internal~${(fbWidth * scale).toInt()}x${(fbHeight * scale).toInt()}"
    }

    private fun updateInferredTargetFps(displayFps: Double, nativeTargetFps: Double) {
        if (nativeTargetFps == 30.0 || nativeTargetFps == 60.0) {
            inferredTargetFps = nativeTargetFps
            if (nativeTargetFps == 30.0) {
                target30SampleCount = 2
                target60SampleCount = 0
            } else {
                target60SampleCount = 2
                target30SampleCount = 0
            }
            return
        }
        if (displayFps >= 26.0 && displayFps <= 34.5) {
            target30SampleCount++
            target60SampleCount = 0
        } else if (displayFps >= 45.0) {
            target60SampleCount++
            target30SampleCount = 0
        }
        if (target30SampleCount >= 2) {
            inferredTargetFps = 30.0
        } else if (target60SampleCount >= 2) {
            inferredTargetFps = 60.0
        }
    }

    private fun resolutionLabel(): String = when (resolutionSetup) {
        0 -> "1/2X"
        1 -> "3/4X"
        2 -> "1X"
        else -> "?"
    }

    private fun aspectRatioLabel(): String = when (aspectRatio) {
        0 -> "16:9"
        1 -> "4:3"
        2 -> "21:9"
        3 -> "16:10"
        4 -> "拉伸全屏"
        else -> "?"
    }

    private fun String.extractDouble(key: String): Double {
        return lineSequence()
            .firstOrNull { it.startsWith("$key=") }
            ?.substringAfter('=')
            ?.toDoubleOrNull() ?: 0.0
    }

    private fun String.extractBoolean(key: String): Boolean {
        return lineSequence()
            .firstOrNull { it.startsWith("$key=") }
            ?.substringAfter('=')
            ?.trim()
            ?.equals("true", ignoreCase = true) ?: false
    }

    private fun String.extractString(key: String): String {
        return lineSequence()
            .firstOrNull { it.startsWith("$key=") }
            ?.substringAfter('=')
            ?.trim()
            .orEmpty()
    }

    private fun formatMs(ms: Double): String {
        return if (ms <= 0.0) "--" else if (ms >= 1000.0) {
            String.format(Locale.US, "%.1fs", ms / 1000.0)
        } else {
            String.format(Locale.US, "%.0fms", ms)
        }
    }

    private fun effectivePreferNce(): Boolean = preferNce

    private fun applyPerGameCompatibilityProfile(
        allowGraphicsCompatOverride: Boolean
    ): String {
        val key = "$gamePath\n$originalGamePath".lowercase(Locale.US)
        val notes = mutableListOf<String>()
        if ("metal dogs" in key || "0100a6e01681c000" in key) {
            if (allowGraphicsCompatOverride && !graphicsCompat) {
                graphicsCompat = true
                notes += "metal-dogs: graphicsCompat=true; stable loading/render path"
            } else {
                notes += "metal-dogs: detected; graphicsCompat=$graphicsCompat; explicitOrAlreadySet"
            }
        }
        if ("kirbystar" in key || "kirby star" in key || "kirby" in key || "星之卡比" in key || "01007e3006dda000" in key) {
            // Kirby used to be forced to Dynarmic after early Android NCE crashes.  Now that
            // the core has the halt-reason bitmask fix and deferred NCE signal handling, do
            // not silently override an explicit NCE request: keeping the request visible is
            // necessary for per-title NCE probes and for building the whitelist/blacklist.
            if (preferNce) {
                notes += "kirby: NCE probe allowed; watch load/logs and fall back manually if unstable"
            } else {
                notes += "kirby: detected; NCE off; stable Dynarmic path"
            }
        }
        return notes.ifEmpty { listOf("none") }.joinToString("; ")
    }

    private fun nceButtonLabel(): String = when {
        preferNce && ANDROID_NCE_STABILITY_GUARD -> "NCE 请求/保护关"
        preferNce -> "NCE 请求"
        else -> "NCE 关闭"
    }

    private fun graphicsCompatButtonLabel(): String = if (graphicsCompat) {
        "图形兼容 开"
    } else {
        "图形兼容 关"
    }

    private fun perfHudButtonLabel(): String = if (perfHudDetailed) {
        "性能HUD 详细"
    } else {
        "性能HUD 精简"
    }

    private fun driverOverlayLabel(): String {
        val summary = runCatching { GpuDriverHelper.summaryText() }.getOrDefault("")
        val name = summary.extractString("lastDriver").ifBlank { "system/default" }
        val library = summary.extractString("lastDriverLibrary")
        return if (library.isBlank()) name else "$name/$library"
    }

    private fun nceStatusLabel(nativeStats: String = ""): String {
        val actual = if (nativeStats.lineSequence().any { it.startsWith("nceEnabled=") }) {
            nativeStats.extractBoolean("nceEnabled")
        } else {
            lastNceActual
        }
        return when {
            actual -> "NCE 实际开"
            preferNce && ANDROID_NCE_STABILITY_GUARD -> "NCE 保护关闭"
            preferNce -> "NCE 请求/未启用"
            else -> "NCE 关"
        }
    }

    private fun Double.format1(): String = if (this <= 0.0) "--" else String.format(Locale.US, "%.1f", this)
    private fun Double.format0(): String = if (this <= 0.0) "--" else String.format(Locale.US, "%.0f", this)
    private fun Double.formatTargetFps(): String = String.format(Locale.US, "%.0f", this)
    private fun Float.format2(): String = String.format(Locale.US, "%.2f", this)

    private fun readDeviceTemperatureText(): String {
        val now = System.currentTimeMillis()
        if (now - lastTemperatureReadMs < 5000L) {
            return cachedTemperatureText
        }
        lastTemperatureReadMs = now
        val batteryTemp = runCatching {
            val intent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val deciC = intent?.getIntExtra("temperature", 0) ?: 0
            if (deciC > 0) deciC / 10.0 else 0.0
        }.getOrDefault(0.0)
        cachedTemperatureText = if (batteryTemp > 0.0) String.format(Locale.US, "%.1f℃ batt", batteryTemp) else "--"
        return cachedTemperatureText
    }

    private fun readThermalZoneTemperature(): Double {
        val candidates = File("/sys/class/thermal").listFiles()
            ?.filter { it.name.startsWith("thermal_zone") }
            .orEmpty()
        for (zone in candidates) {
            val type = File(zone, "type").readTextOrNull().lowercase(Locale.US)
            if (type.contains("gpu") || type.contains("cpu") || type.contains("soc") || type.contains("skin")) {
                val raw = File(zone, "temp").readTextOrNull().trim().toDoubleOrNull() ?: continue
                return if (raw > 1000) raw / 1000.0 else raw
            }
        }
        return 0.0
    }

    private fun File.readTextOrNull(): String = runCatching { readText().trim() }.getOrDefault("")

    private fun stopEmulationAndFinish() {
        appendSessionLog("stop-button-before", includeSnapshot = true)
        val result = stopNativeRuntime()
        statusView.append("\n\nstopping:\n$result")
        appendSessionLog("stop-button-after", result, includeSnapshot = true)
        saveDiagnosticsToNsLogs("stop-button")
        finish()
    }

    private fun registerDebugInputReceiver() {
        val filter = IntentFilter(ACTION_DEBUG_INPUT)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(debugInputReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(debugInputReceiver, filter)
        }
    }

    private fun switchButtonOrdinal(name: String): Int {
        return when (name.trim().lowercase(Locale.US)) {
            "a" -> SWITCH_BUTTON_A
            "b" -> SWITCH_BUTTON_B
            "x" -> SWITCH_BUTTON_X
            "y" -> SWITCH_BUTTON_Y
            "l", "lb" -> SWITCH_BUTTON_L
            "r", "rb" -> SWITCH_BUTTON_R
            "zl", "lt", "l2" -> SWITCH_BUTTON_ZL
            "zr", "rt", "r2" -> SWITCH_BUTTON_ZR
            "l3", "lstick", "leftstick" -> SWITCH_BUTTON_LSTICK
            "r3", "rstick", "rightstick" -> SWITCH_BUTTON_RSTICK
            "+", "plus", "start" -> SWITCH_BUTTON_PLUS
            "-", "minus", "select" -> SWITCH_BUTTON_MINUS
            "left", "dleft", "←" -> SWITCH_BUTTON_LEFT
            "up", "dup", "↑" -> SWITCH_BUTTON_UP
            "right", "dright", "→" -> SWITCH_BUTTON_RIGHT
            "down", "ddown", "↓" -> SWITCH_BUTTON_DOWN
            else -> -1
        }
    }

    private data class DebugAnalogInput(
        val label: String,
        val stick: Int,
        val x: Float,
        val y: Float
    )

    private fun debugAnalogFromName(name: String): DebugAnalogInput? {
        return when (name.trim().lowercase(Locale.US)) {
            "lsup", "lup", "leftstickup" -> DebugAnalogInput("debug-ls-up", 0, 0f, -1f)
            "lsdown", "ldown", "leftstickdown" -> DebugAnalogInput("debug-ls-down", 0, 0f, 1f)
            "lsleft", "lleft", "leftstickleft" -> DebugAnalogInput("debug-ls-left", 0, -1f, 0f)
            "lsright", "lright", "leftstickright" -> DebugAnalogInput("debug-ls-right", 0, 1f, 0f)
            "rsup", "rup", "rightstickup" -> DebugAnalogInput("debug-rs-up", 1, 0f, -1f)
            "rsdown", "rdown", "rightstickdown" -> DebugAnalogInput("debug-rs-down", 1, 0f, 1f)
            "rsleft", "rleft", "rightstickleft" -> DebugAnalogInput("debug-rs-left", 1, -1f, 0f)
            "rsright", "rright", "rightstickright" -> DebugAnalogInput("debug-rs-right", 1, 1f, 0f)
            else -> null
        }
    }

    private fun LinearLayout.addTouchButton(button: Button, width: Int, height: Int) {
        button.layoutParams = LinearLayout.LayoutParams(scaledControlSize(width), scaledControlSize(height)).apply {
            setMargins(4, 4, 4, 4)
        }
        addView(button)
    }

    private fun LinearLayout.addSpacer(size: Int) {
        addView(View(this@EmulationActivity).apply {
            val scaled = scaledControlSize(size)
            layoutParams = LinearLayout.LayoutParams(scaled, scaled).apply {
                setMargins(4, 4, 4, 4)
            }
        })
    }
    private fun createInputButton(label: String, buttonOrdinal: Int): Button {
        return Button(this).apply {
            text = label
            textSize = when { label.length > 5 -> 11f; label.length > 2 -> 14f; else -> 18f }
            applyControlButtonStyle()
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        text = label
                        alpha = 0.96f
                        setTextColor(0xffffffff.toInt())
                        setBackgroundColor(0xcc1b8f3a.toInt())
                        appendInputResult(label, buttonOrdinal, true)
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        text = label
                        appendInputResult(label, buttonOrdinal, false)
                        if (buttonOrdinal == SWITCH_BUTTON_A || buttonOrdinal == SWITCH_BUTTON_X) {
                            postDelayed({
                                launchPendingNextLoad("after-$label")
                            }, 250)
                        }
                        postDelayed({
                            text = label
                            alpha = (CONTROL_ALPHA * touchOpacityScale).coerceIn(0.30f, 1.0f)
                            setBackgroundColor(CONTROL_BG_COLOR)
                        }, CONTROL_RELEASE_DELAY_MS)
                        true
                    }
                    else -> false
                }
            }
        }
    }

    private fun Button.applyControlButtonStyle() {
        isAllCaps = false
        alpha = (CONTROL_ALPHA * touchOpacityScale).coerceIn(0.30f, 1.0f)
        setTextColor(0xffffffff.toInt())
        minHeight = 0
        minimumHeight = 0
        setIncludeFontPadding(false)
        setPadding(4, 0, 4, 0)
        setBackgroundColor(CONTROL_BG_COLOR)
    }

    private fun createAnalogStickView(label: String, stickIndex: Int): View {
        return object : View(this) {
            private val basePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xaa101010.toInt()
                style = Paint.Style.FILL
            }
            private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xddffffff.toInt()
                style = Paint.Style.STROKE
                strokeWidth = 8f
            }
            private val knobPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xcc2f7dff.toInt()
                style = Paint.Style.FILL
            }
            private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = 0xffffffff.toInt()
                textAlign = Paint.Align.CENTER
                textSize = 40f
                isFakeBoldText = true
            }
            private var knobX = 0f
            private var knobY = 0f
            private var lastSentX = 9f
            private var lastSentY = 9f

            override fun onDraw(canvas: Canvas) {
                super.onDraw(canvas)
                val cx = width / 2f
                val cy = height / 2f
                val radius = (minOf(width, height) / 2f) - 4f
                if (basePaint.shader == null && radius > 0f) {
                    basePaint.shader = RadialGradient(
                        cx, cy, radius,
                        intArrayOf(0xaa202020.toInt(), 0x66101010),
                        floatArrayOf(0f, 1f),
                        Shader.TileMode.CLAMP
                    )
                }
                canvas.drawCircle(cx, cy, radius, basePaint)
                canvas.drawCircle(cx, cy, radius, ringPaint)
                canvas.drawCircle(cx + knobX * radius * 0.56f, cy + knobY * radius * 0.56f, radius * 0.44f, knobPaint)
                canvas.drawText(label, cx, cy + 12f, labelPaint)
            }

            override fun onTouchEvent(event: MotionEvent): Boolean {
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                        val cx = width / 2f
                        val cy = height / 2f
                        val maxRadius = (minOf(width, height) / 2f).coerceAtLeast(1f)
                        var x = (event.x - cx) / maxRadius
                        var y = (event.y - cy) / maxRadius
                        val length = hypot(x, y)
                        if (length > 1f) {
                            x /= length
                            y /= length
                        }
                        if (kotlin.math.abs(x) < ANALOG_DEADZONE) x = 0f
                        if (kotlin.math.abs(y) < ANALOG_DEADZONE) y = 0f
                        knobX = x
                        knobY = y
                        sendAnalogInput(label, stickIndex, x, y, force = false)
                        invalidate()
                        return true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        knobX = 0f
                        knobY = 0f
                        sendAnalogInput(label, stickIndex, 0f, 0f, force = true)
                        invalidate()
                        return true
                    }
                }
                return true
            }

            private fun sendAnalogInput(label: String, stickIndex: Int, x: Float, y: Float, force: Boolean) {
                if (!force && kotlin.math.abs(x - lastSentX) < ANALOG_SEND_THRESHOLD && kotlin.math.abs(y - lastSentY) < ANALOG_SEND_THRESHOLD) {
                    return
                }
                lastSentX = x
                lastSentY = y
                appendAnalogResult(label, stickIndex, x, y)
            }
        }
    }

    private fun appendAnalogResult(label: String, stickIndex: Int, x: Float, y: Float) {
        // Android touch coordinates grow downward, while Switch guest stick Y expects up-positive.
        // Invert Y for both left and right sticks; keep the visual knob following the finger.
        val nativeY = -y
        val result = runCatching {
            NativeLibrary.setPlayerAnalog(0, stickIndex, x, nativeY)
        }.getOrElse { error ->
            "analog=exception\n${error.stackTraceToString()}"
        }
        val firstLine = result.lineSequence().firstOrNull().orEmpty()
        inputStateView.text = "analog $label  x=${x.format2()} y=${y.format2()} nativeY=${nativeY.format2()} stick=$stickIndex ${firstLine}"
        val now = System.currentTimeMillis()
        if ((x == 0f && y == 0f) || now - lastAnalogSessionLogMs >= 1000L) {
            lastAnalogSessionLogMs = now
            appendSessionLog(
                "analog-$label",
                "stick=$stickIndex\nx=${x.format2()}\ny=${y.format2()}\n$result",
                includeSnapshot = false
            )
        }
    }

    private fun appendInputResult(label: String, buttonOrdinal: Int, pressed: Boolean) {
        val action = if (pressed) "down" else "up"
        val result = runCatching {
            NativeLibrary.setPlayerButton(0, buttonOrdinal, pressed)
        }.getOrElse { error ->
            "input=exception\n${error.stackTraceToString()}"
        }
        val firstLine = result.lineSequence().firstOrNull().orEmpty()
        inputStateView.text = buildString {
            append("input  ")
            append(label)
            append("  ")
            append(if (pressed) "按下/down" else "松开/up")
            append("  ordinal=")
            append(buttonOrdinal)
            if (firstLine.isNotBlank()) {
                append("  ")
                append(firstLine)
            }
        }
        statusView.append("\ninput $label $action ordinal=$buttonOrdinal:\n$result")
        appendSessionLog("input-$label-$action", "ordinal=$buttonOrdinal\n$result")
    }

    private fun launchPendingNextLoad(reason: String) {
        val result = runCatching {
            NativeLibrary.launchPendingNextLoad()
        }.getOrElse { error ->
            "launchPendingNextLoad=exception\n${error.stackTraceToString()}"
        }
        val launched = result.contains("launchFresh=LoadRom accepted")
        val skipped = result.contains("launchPendingNextLoad=skipped")
        inputStateView.text = when {
            launched -> "next-load: 已启动新 NRO ($reason)"
            skipped -> "next-load: 暂无待启动目标 ($reason)"
            else -> "next-load: 失败/未知 ($reason)"
        }
        statusView.append("\nnext-load $reason:\n$result")
        appendSessionLog("next-load-$reason", result, includeSnapshot = launched)
        if (launched) {
            overlayScroll.visibility = android.view.View.GONE
            toggleOverlayButton.text = "显示诊断"
        }
    }

    private fun buildDiagnosticsText(reason: String): String {
        return buildString {
            appendLine("===== NxEmu Android Diagnostics =====")
            appendLine("reason=$reason")
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
            appendLine(GpuDriverHelper.statusText())
            appendLine("originalGamePath=$originalGamePath")
            appendLine("pathRepair:")
            appendLine(gamePathRepairStatus)
            appendLine("gamePath=$gamePath")
            appendLine("gameExists=${NativeLibrary.exists(gamePath)}")
            appendLine("gameSize=${NativeLibrary.getSize(gamePath)}")
            appendLine("surfaceAlive=$surfaceAlive")
            appendLine("bootStarted=$bootStarted")
            appendLine("stopRequested=${stopRequested.get()}")
            appendLine("inputState=${inputStateView.text}")
            appendLine("perfOverlay=${perfView.text}")
            appendLine("lastSavedLogPath=${lastSavedLogPath.ifBlank { "none" }}")
            appendLine("autoOutputLog=$autoOutputLog")
            appendLine("sessionLogFile=${sessionLogFile?.absolutePath ?: "none"}")
            appendLine("frameSkip=$frameSkip")
            appendLine("resolution=${resolutionLabel()} ($resolutionSetup)")
            appendLine("aspectRatio=${aspectRatioLabel()} ($aspectRatio)")
            appendLine("graphicsCompat=$graphicsCompat")
            appendLine("preferNce=$preferNce")
            appendLine("effectivePreferNce=${effectivePreferNce()}")
            appendLine("androidNceGuard=$ANDROID_NCE_STABILITY_GUARD")
            appendLine("perfHudDetailed=$perfHudDetailed")
            appendLine()
            appendLine("----- Native performanceStats -----")
            appendLine(runCatching { NativeLibrary.getPerformanceStats() }.getOrElse { it.stackTraceToString() })
            appendLine()
            appendLine("----- Native loadProgress -----")
            appendLine(runCatching { NativeLibrary.getLoadProgress() }.getOrElse { it.stackTraceToString() })
            appendLine()
            appendLine("----- Native runtimeStatus -----")
            appendLine(runCatching { NativeLibrary.runtimeStatus() }.getOrElse { it.stackTraceToString() })
            appendLine()
            appendLine("----- Page text -----")
            appendLine(statusView.text.toString())
            appendLine("===== End NxEmu Android Diagnostics =====")
        }
    }

    private fun copyDiagnosticsToClipboard() {
        val text = buildDiagnosticsText("copy-button")

        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("nxemu diagnostics", text))
        val saved = saveDiagnosticsToNsLogs("copy-button", text)
        appendSessionLog("copy-button", text.take(12000), includeSnapshot = false)
        Toast.makeText(
            this,
            if (saved.isNotBlank()) "诊断已复制并保存: $saved" else "诊断日志已复制，可直接粘贴发我",
            Toast.LENGTH_LONG
        ).show()
    }

    private fun saveDiagnosticsToNsLogs(reason: String, prebuiltText: String? = null): String {
        val text = prebuiltText ?: runCatching { buildDiagnosticsText(reason) }
            .getOrElse { "diagnostics-build-failed\nreason=$reason\n${it.stackTraceToString()}" }
        val timestamp = SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(Date())
        val safeReason = reason.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val targets = listOf(
            File("/sdcard/ns/logs"),
            File("/storage/emulated/0/ns/logs"),
            File(getExternalFilesDir(null), "logs"),
            File(filesDir, "logs"),
        )
        for (dir in targets) {
            val result = runCatching {
                dir.mkdirs()
                val file = File(dir, "nxemu-${timestamp}-${safeReason}.txt")
                file.writeText(text)
                file.absolutePath
            }.getOrNull()
            if (!result.isNullOrBlank()) {
                lastSavedLogPath = result
                Log.i("NxEmuAndroid", "diagnosticsSaved reason=$reason path=$result")
                appendSessionLog("diagnostics-saved-$reason", "path=$result", includeSnapshot = false)
                return result
            }
        }
        Log.e("NxEmuAndroid", "diagnosticsSaveFailed reason=$reason")
        return ""
    }

    private fun createSessionLogFile(): File? {
        val timestamp = SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(Date())
        val gameName = gamePath
            .substringAfterLast('/')
            .substringAfterLast(':')
            .ifBlank { "unknown-game" }
            .replace(Regex("[^A-Za-z0-9._-]"), "_")
            .take(80)
        val targets = listOf(
            File("/sdcard/ns/logs"),
            File("/storage/emulated/0/ns/logs"),
            File(getExternalFilesDir(null), "logs"),
            File(filesDir, "logs"),
        )
        for (dir in targets) {
            val file = runCatching {
                dir.mkdirs()
                File(dir, "nxemu-session-$timestamp-$gameName.txt").also {
                    it.writeText(
                        buildString {
                            appendLine("===== NxEmu Android Auto Session Log =====")
                            appendLine("created=${SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS Z", Locale.US).format(Date())}")
                            appendLine("package=$packageName")
                            appendLine("device=${Build.MANUFACTURER} ${Build.MODEL}")
                            appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT}")
                            appendLine("abis=${Build.SUPPORTED_ABIS.joinToString()}")
                            appendLine("nativeLibraryDir=${applicationInfo.nativeLibraryDir}")
                            appendLine("gamePath=$gamePath")
                            appendLine("originalGamePath=$originalGamePath")
                            appendLine()
                        }
                    )
                }
            }.getOrNull()
            if (file != null) {
                lastSavedLogPath = file.absolutePath
                Log.i("NxEmuAndroid", "sessionLogCreated path=${file.absolutePath}")
                return file
            }
        }
        Log.e("NxEmuAndroid", "sessionLogCreateFailed")
        return null
    }

    private fun appendSessionLogAsync(stage: String, extra: String = "", includeSnapshot: Boolean = false) {
        Thread({ appendSessionLog(stage, extra, includeSnapshot) }, "NxEmuLog-$stage").start()
    }
    private fun appendSessionLog(stage: String, extra: String = "", includeSnapshot: Boolean = false) {
        if (!autoOutputLog) return
        val file = sessionLogFile ?: createSessionLogFile()?.also { sessionLogFile = it } ?: return
        val timestamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS Z", Locale.US).format(Date())
        val text = buildString {
            appendLine("----- $timestamp [$stage] -----")
            appendLine("thread=${Thread.currentThread().name}")
            if (extra.isNotBlank()) {
                appendLine(extra)
            }
            if (includeSnapshot) {
                appendLine()
                appendLine("snapshot:")
                appendLine("surfaceAlive=$surfaceAlive bootStarted=$bootStarted stopRequested=${stopRequested.get()}")
                if (::perfView.isInitialized) appendLine("perfOverlay=${perfView.text}")
                appendLine("performanceStats:")
                appendLine(runCatching { NativeLibrary.getPerformanceStats() }.getOrElse { it.stackTraceToString() })
                appendLine("loadProgress:")
                appendLine(runCatching { NativeLibrary.getLoadProgress() }.getOrElse { it.stackTraceToString() })
                appendLine("runtimeStatus:")
                appendLine(runCatching { NativeLibrary.runtimeStatus() }.getOrElse { it.stackTraceToString() })
            }
            appendLine()
        }
        synchronized(sessionLogLock) {
            runCatching {
                file.appendText(text)
            }.onFailure {
                Log.e("NxEmuAndroid", "sessionLogAppendFailed stage=$stage path=${file.absolutePath}", it)
            }
        }
    }

    private fun stopNativeRuntime(): String {
        if (!stopRequested.compareAndSet(false, true)) {
            return "shutdown=already requested"
        }
        return buildString {
            // 先停 emulation/modules，再释放 Surface。之前相反顺序会让 nvnflinger/BufferQueue 更容易在停机时噪声甚至断言。
            appendLine(NativeLibrary.shutdownRuntime())
            append(NativeLibrary.setSurface(null))
        }
    }

    companion object {
        const val EXTRA_GAME_PATH = "org.nxemu.app.EXTRA_GAME_PATH"
        const val EXTRA_GAME_NAME = "org.nxemu.app.EXTRA_GAME_NAME"
        const val EXTRA_AUTO_OUTPUT_LOG = "org.nxemu.app.EXTRA_AUTO_OUTPUT_LOG"
        const val EXTRA_PREFER_NCE = "org.nxemu.app.EXTRA_PREFER_NCE"
        const val EXTRA_PERF_HUD_DETAILED = "org.nxemu.app.EXTRA_PERF_HUD_DETAILED"
        const val EXTRA_FRAME_SKIP = "org.nxemu.app.EXTRA_FRAME_SKIP"
        const val EXTRA_RESOLUTION_SETUP = "org.nxemu.app.EXTRA_RESOLUTION_SETUP"
        const val EXTRA_ASPECT_RATIO = "org.nxemu.app.EXTRA_ASPECT_RATIO"
        const val EXTRA_GRAPHICS_COMPAT = "org.nxemu.app.EXTRA_GRAPHICS_COMPAT"
        const val EXTRA_GPU_DRIVER_SOURCE = "org.nxemu.app.EXTRA_GPU_DRIVER_SOURCE"
        private const val ACTION_DEBUG_INPUT = "org.nxemu.app.DEBUG_INPUT"
        private const val EXTRA_DEBUG_BUTTON = "button"
        private const val EXTRA_DEBUG_HOLD_MS = "holdMs"
        // Java 只负责把“请求 NCE”传给 native；native 侧仍用 debug.nxemu.nce
        // 实验属性决定是否真正越过稳定保护，避免普通用户误开导致崩溃。
        private const val ANDROID_NCE_STABILITY_GUARD = false
        private const val CONTROL_ALPHA = 0.82f
        private const val TOUCH_ALPHA = 0.88f
        private const val STICK_ALPHA = 0.90f
        private const val TOOLBAR_ALPHA = 0.68f
        private val CONTROL_BG_COLOR = 0xdd101010.toInt()
        private const val CONTROL_RELEASE_DELAY_MS = 320L
        private const val BACK_EXIT_CONFIRM_MS = 2200L
        private const val DEFAULT_FRAME_SKIP = 0
        private const val PERF_SNAPSHOT_INTERVAL_TICKS = 5
        private const val RENDER_STALL_LOG_AFTER_MS = 5000L
        private const val SETTINGS_BAR_HEIGHT = 46
        private const val SETTINGS_DRAWER_WIDTH = 420
        private const val SETTINGS_DRAWER_BUTTON_HEIGHT = 72
        private const val EDEN_HANDLE_SIZE = 72
        private const val EDEN_HANDLE_ALPHA = 0.74f
        private const val PAUSE_MENU_WIDTH = 760
        private const val DRAWER_ALPHA = 0.90f
        private const val DRAWER_AUTO_HIDE_MS = 9000L
        private val DRAWER_BG_COLOR = 0xee101014.toInt()
        private const val DPAD_BUTTON_SIZE = 126
        private const val FACE_BUTTON_SIZE = 134
        private const val SMALL_BUTTON_WIDTH = 226
        private const val SMALL_BUTTON_HEIGHT = 104
        private const val CENTER_BUTTON_GAP = 24
        private const val SHOULDER_BUTTON_WIDTH = 154
        private const val SHOULDER_BUTTON_HEIGHT = 88
        private const val STICK_CLICK_BUTTON_SIZE = 110
        // Larger touch/view area: users reported fingers easily slide out of the old stick circle on glass.
        private const val ANALOG_STICK_SIZE = 430
        private const val BOOT_SURFACE_SETTLE_DELAY_MS = 750L
        private const val SWITCH_BUTTON_A = 0
        private const val SWITCH_BUTTON_B = 1
        private const val SWITCH_BUTTON_X = 2
        private const val SWITCH_BUTTON_Y = 3
        private const val SWITCH_BUTTON_LSTICK = 4
        private const val SWITCH_BUTTON_RSTICK = 5
        private const val SWITCH_BUTTON_L = 6
        private const val SWITCH_BUTTON_R = 7
        private const val SWITCH_BUTTON_ZL = 8
        private const val SWITCH_BUTTON_ZR = 9
        private const val SWITCH_BUTTON_PLUS = 10
        private const val SWITCH_BUTTON_MINUS = 11
        private const val SWITCH_BUTTON_LEFT = 12
        private const val SWITCH_BUTTON_UP = 13
        private const val SWITCH_BUTTON_RIGHT = 14
        private const val SWITCH_BUTTON_DOWN = 15
        private const val SWITCH_ANALOG_LEFT = 0
        private const val SWITCH_ANALOG_RIGHT = 1
        private const val ANALOG_DEADZONE = 0.08f
        private const val ANALOG_SEND_THRESHOLD = 0.045f
    }
}









