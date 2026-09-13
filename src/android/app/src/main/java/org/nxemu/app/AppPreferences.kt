package org.nxemu.app

import android.content.Context
import java.security.MessageDigest
import java.util.Locale

object AppPreferences {
    private const val PREFS_NAME = "nxemu_android_settings"
    private const val KEY_LAST_GAME_PATH = "last_game_path"
    private const val KEY_LAST_GAME_NAME = "last_game_name"
    private const val KEY_LAST_GAME_FOLDER = "last_game_folder"
    private const val KEY_FRAME_SKIP = "frame_skip"
    private const val KEY_RESOLUTION_SETUP = "resolution_setup"
    private const val KEY_ASPECT_RATIO = "aspect_ratio"
    private const val KEY_GRAPHICS_COMPAT = "graphics_compat"
    private const val KEY_PREFER_NCE = "prefer_nce"
    private const val KEY_PERF_HUD_DETAILED = "perf_hud_detailed"
    private const val KEY_AUTO_OUTPUT_LOG = "auto_output_log"
    private const val KEY_STABLE_DEFAULTS_VERSION = "stable_defaults_version"
    private const val STABLE_DEFAULTS_VERSION = 2
    private const val KEY_HOME_VIEW_MODE = "home_view_mode"
    private const val KEY_HOME_SORT_MODE = "home_sort_mode"
    private const val KEY_TOUCH_LAYOUT_PRESET = "touch_layout_preset"
    private const val KEY_TOUCH_OPACITY = "touch_opacity"
    private const val KEY_TOUCH_INITIAL_VISIBLE = "touch_initial_visible"
    private const val KEY_TOUCH_AUTO_HIDE = "touch_auto_hide"
    private const val KEY_SHOW_RAW_SWITCH_CONTAINERS = "show_raw_switch_containers"

    data class GameProfile(
        val enabled: Boolean,
        val frameSkip: Int,
        val resolutionSetup: Int,
        val aspectRatio: Int,
        val graphicsCompat: Boolean,
        val preferNce: Boolean,
        val perfHudDetailed: Boolean,
        val autoOutputLog: Boolean,
        val driverSource: String
    )

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    private fun ensureStableDefaults(context: Context) {
        val p = prefs(context)
        if (p.getInt(KEY_STABLE_DEFAULTS_VERSION, 0) >= STABLE_DEFAULTS_VERSION) return
        val editor = p.edit().putBoolean(KEY_PREFER_NCE, false)
        // Older test packages defaulted global/per-game NCE to true. Current Android NCE is
        // still an experiment and can make titles return to the menu, so reset saved requests
        // once for the stable package. Users can explicitly re-enable NCE after this migration.
        for (key in p.all.keys) {
            if (key.endsWith("_prefer_nce")) {
                editor.putBoolean(key, false)
            }
        }
        editor.putInt(KEY_STABLE_DEFAULTS_VERSION, STABLE_DEFAULTS_VERSION).apply()
    }

    fun lastGamePath(context: Context): String = prefs(context).getString(KEY_LAST_GAME_PATH, "").orEmpty()
    fun lastGameName(context: Context): String = prefs(context).getString(KEY_LAST_GAME_NAME, "").orEmpty()
    fun lastGameFolder(context: Context): String = prefs(context).getString(KEY_LAST_GAME_FOLDER, "").orEmpty()
    fun frameSkip(context: Context): Int = prefs(context).getInt(KEY_FRAME_SKIP, 0).coerceIn(0, 4)
    fun resolutionSetup(context: Context): Int = prefs(context).getInt(KEY_RESOLUTION_SETUP, 0).coerceIn(0, 2)
    fun aspectRatio(context: Context): Int = prefs(context).getInt(KEY_ASPECT_RATIO, 4).coerceIn(0, 4)
    fun graphicsCompat(context: Context): Boolean = prefs(context).getBoolean(KEY_GRAPHICS_COMPAT, false)
    fun preferNce(context: Context): Boolean {
        ensureStableDefaults(context)
        return prefs(context).getBoolean(KEY_PREFER_NCE, false)
    }
    fun perfHudDetailed(context: Context): Boolean = prefs(context).getBoolean(KEY_PERF_HUD_DETAILED, false)
    fun autoOutputLog(context: Context): Boolean = prefs(context).getBoolean(KEY_AUTO_OUTPUT_LOG, false)
    fun homeViewMode(context: Context): Int = prefs(context).getInt(KEY_HOME_VIEW_MODE, 0).coerceIn(0, 2)
    fun saveHomeViewMode(context: Context, mode: Int) { prefs(context).edit().putInt(KEY_HOME_VIEW_MODE, mode.coerceIn(0, 2)).apply() }
    fun homeSortMode(context: Context): Int = prefs(context).getInt(KEY_HOME_SORT_MODE, 0).coerceIn(0, 5)
    fun saveHomeSortMode(context: Context, mode: Int) { prefs(context).edit().putInt(KEY_HOME_SORT_MODE, mode.coerceIn(0, 5)).apply() }
    fun touchLayoutPreset(context: Context): Int = prefs(context).getInt(KEY_TOUCH_LAYOUT_PRESET, 1).coerceIn(0, 2)
    fun saveTouchLayoutPreset(context: Context, preset: Int) { prefs(context).edit().putInt(KEY_TOUCH_LAYOUT_PRESET, preset.coerceIn(0, 2)).apply() }
    fun touchOpacity(context: Context): Int = prefs(context).getInt(KEY_TOUCH_OPACITY, 70).coerceIn(30, 100)
    fun saveTouchOpacity(context: Context, opacity: Int) { prefs(context).edit().putInt(KEY_TOUCH_OPACITY, opacity.coerceIn(30, 100)).apply() }
    fun touchInitialVisible(context: Context): Boolean = prefs(context).getBoolean(KEY_TOUCH_INITIAL_VISIBLE, true)
    fun saveTouchInitialVisible(context: Context, visible: Boolean) { prefs(context).edit().putBoolean(KEY_TOUCH_INITIAL_VISIBLE, visible).apply() }
    fun touchAutoHide(context: Context): Boolean = prefs(context).getBoolean(KEY_TOUCH_AUTO_HIDE, false)
    fun saveTouchAutoHide(context: Context, enabled: Boolean) { prefs(context).edit().putBoolean(KEY_TOUCH_AUTO_HIDE, enabled).apply() }
    fun touchPresetLabel(value: Int): String = when (value.coerceIn(0, 2)) { 0 -> "紧凑"; 2 -> "大号"; else -> "标准" }
    fun showRawSwitchContainers(context: Context): Boolean = prefs(context).getBoolean(KEY_SHOW_RAW_SWITCH_CONTAINERS, false)
    fun saveShowRawSwitchContainers(context: Context, show: Boolean) {
        prefs(context).edit().putBoolean(KEY_SHOW_RAW_SWITCH_CONTAINERS, show).apply()
    }

    fun globalGameProfile(context: Context): GameProfile = GameProfile(
        enabled = false,
        frameSkip = frameSkip(context),
        resolutionSetup = resolutionSetup(context),
        aspectRatio = aspectRatio(context),
        graphicsCompat = graphicsCompat(context),
        preferNce = preferNce(context),
        perfHudDetailed = perfHudDetailed(context),
        autoOutputLog = autoOutputLog(context),
        driverSource = ""
    )

    fun perGameKey(path: String): String {
        val digest = MessageDigest.getInstance("SHA-1")
            .digest(path.trim().lowercase(Locale.US).toByteArray(Charsets.UTF_8))
        return digest.joinToString("") { "%02x".format(it) }
    }

    fun perGameProfile(context: Context, path: String): GameProfile {
        if (path.isBlank()) {
            return globalGameProfile(context)
        }
        val global = globalGameProfile(context)
        val prefix = "game_${perGameKey(path)}_"
        val p = prefs(context)
        val profile = GameProfile(
            enabled = p.getBoolean(prefix + "enabled", false),
            frameSkip = p.getInt(prefix + "frame_skip", global.frameSkip).coerceIn(0, 4),
            resolutionSetup = p.getInt(prefix + "resolution_setup", global.resolutionSetup).coerceIn(0, 2),
            aspectRatio = p.getInt(prefix + "aspect_ratio", global.aspectRatio).coerceIn(0, 4),
            graphicsCompat = p.getBoolean(prefix + "graphics_compat", global.graphicsCompat),
            preferNce = p.getBoolean(prefix + "prefer_nce", global.preferNce),
            perfHudDetailed = p.getBoolean(prefix + "perf_hud_detailed", global.perfHudDetailed),
            autoOutputLog = p.getBoolean(prefix + "auto_output_log", global.autoOutputLog),
            driverSource = p.getString(prefix + "driver_source", "").orEmpty()
        )
        return profile
    }

    fun savePerGameProfile(context: Context, path: String, profile: GameProfile) {
        if (path.isBlank()) return
        val prefix = "game_${perGameKey(path)}_"
        prefs(context).edit()
            .putBoolean(prefix + "enabled", profile.enabled)
            .putInt(prefix + "frame_skip", profile.frameSkip.coerceIn(0, 4))
            .putInt(prefix + "resolution_setup", profile.resolutionSetup.coerceIn(0, 2))
            .putInt(prefix + "aspect_ratio", profile.aspectRatio.coerceIn(0, 4))
            .putBoolean(prefix + "graphics_compat", profile.graphicsCompat)
            .putBoolean(prefix + "prefer_nce", profile.preferNce)
            .putBoolean(prefix + "perf_hud_detailed", profile.perfHudDetailed)
            .putBoolean(prefix + "auto_output_log", profile.autoOutputLog)
            .putString(prefix + "driver_source", profile.driverSource)
            .apply()
    }

    fun resetPerGameProfile(context: Context, path: String) {
        if (path.isBlank()) return
        val prefix = "game_${perGameKey(path)}_"
        prefs(context).edit()
            .remove(prefix + "enabled")
            .remove(prefix + "frame_skip")
            .remove(prefix + "resolution_setup")
            .remove(prefix + "aspect_ratio")
            .remove(prefix + "graphics_compat")
            .remove(prefix + "prefer_nce")
            .remove(prefix + "perf_hud_detailed")
            .remove(prefix + "auto_output_log")
            .remove(prefix + "driver_source")
            .apply()
    }

    fun profileSummary(context: Context, path: String): String {
        val profile = perGameProfile(context, path)
        if (!profile.enabled) {
            return "使用全局配置"
        }
        return buildString {
            append("独立配置 · ")
            append("skip=${profile.frameSkip} · ")
            append("res=${resolutionLabel(profile.resolutionSetup)} · ")
            append("画面=${aspectLabel(profile.aspectRatio)} · ")
            append("compat=${if (profile.graphicsCompat) "开" else "关"} · ")
            append("NCE=${if (profile.preferNce) "开" else "关"}")
            if (profile.driverSource.isNotBlank()) {
                append(" · 独立驱动")
            }
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

    fun saveLastGame(context: Context, path: String?, name: String? = null) {
        prefs(context).edit()
            .putString(KEY_LAST_GAME_PATH, path.orEmpty())
            .putString(KEY_LAST_GAME_NAME, name.orEmpty())
            .apply()
    }

    fun saveLastGameFolder(context: Context, folder: String?) {
        prefs(context).edit().putString(KEY_LAST_GAME_FOLDER, folder.orEmpty()).apply()
    }

    fun savePerformance(
        context: Context,
        frameSkip: Int,
        resolutionSetup: Int,
        aspectRatio: Int,
        graphicsCompat: Boolean,
        preferNce: Boolean
    ) {
        prefs(context).edit()
            .putInt(KEY_FRAME_SKIP, frameSkip.coerceIn(0, 4))
            .putInt(KEY_RESOLUTION_SETUP, resolutionSetup.coerceIn(0, 2))
            .putInt(KEY_ASPECT_RATIO, aspectRatio.coerceIn(0, 4))
            .putBoolean(KEY_GRAPHICS_COMPAT, graphicsCompat)
            .putBoolean(KEY_PREFER_NCE, preferNce)
            .apply()
    }

    fun saveAutoOutputLog(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_AUTO_OUTPUT_LOG, enabled).apply()
    }

    fun savePerfHudDetailed(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_PERF_HUD_DETAILED, enabled).apply()
    }

    fun statusText(context: Context): String = buildString {
        appendLine("Saved settings:")
        appendLine("lastGame=${lastGamePath(context).ifBlank { "none" }}")
        appendLine("lastGameName=${lastGameName(context).ifBlank { "none" }}")
        appendLine("lastFolder=${lastGameFolder(context).ifBlank { "none" }}")
        appendLine("frameSkip=${frameSkip(context)}")
        appendLine("resolutionSetup=${resolutionSetup(context)}")
        appendLine("aspectRatio=${aspectRatio(context)}")
        appendLine("graphicsCompat=${graphicsCompat(context)}")
        appendLine("preferNce=${preferNce(context)}")
        appendLine("perfHudDetailed=${perfHudDetailed(context)}")
        appendLine("autoOutputLog=${autoOutputLog(context)}")
        appendLine("homeViewMode=${homeViewMode(context)}")
        appendLine("homeSortMode=${homeSortMode(context)}")
        appendLine("touchLayoutPreset=${touchLayoutPreset(context)}(${touchPresetLabel(touchLayoutPreset(context))})")
        appendLine("touchOpacity=${touchOpacity(context)}")
        appendLine("touchInitialVisible=${touchInitialVisible(context)}")
        appendLine("touchAutoHide=${touchAutoHide(context)}")
        appendLine("showRawSwitchContainers=${showRawSwitchContainers(context)}")
    }
}
