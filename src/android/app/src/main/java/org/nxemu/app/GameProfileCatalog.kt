package org.nxemu.app

import android.content.Context
import android.os.Environment
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.Locale

object GameProfileCatalog {
    private const val PROFILE_FILE_NAME = "nxemu_game_profiles.json"

    data class Preset(
        val id: String,
        val title: String,
        val description: String,
        val profile: AppPreferences.GameProfile,
        val source: String
    )

    fun profileFile(context: Context): File {
        val external = File(Environment.getExternalStorageDirectory(), "ns/config/$PROFILE_FILE_NAME")
        return if (external.parentFile?.exists() == true || external.parentFile?.mkdirs() == true) {
            external
        } else {
            File(context.filesDir, "config/$PROFILE_FILE_NAME")
        }
    }

    fun ensureProfileFile(context: Context): File {
        val file = profileFile(context)
        if (!file.exists() || file.length() <= 0) {
            file.parentFile?.mkdirs()
            file.writeText(defaultJson(), Charsets.UTF_8)
        } else {
            migrateBuiltinProfiles(file)
        }
        return file
    }

    private fun migrateBuiltinProfiles(file: File) {
        runCatching {
            val root = JSONObject(file.readText(Charsets.UTF_8))
            val profiles = root.optJSONArray("profiles") ?: return
            var changed = false
            for (i in 0 until profiles.length()) {
                val item = profiles.optJSONObject(i) ?: continue
                val id = item.optString("id")
                if (id != "kirby-star-allies-26-2" && id != "metal-dogs-stable") continue
                val settings = item.optJSONObject("settings") ?: JSONObject().also {
                    item.put("settings", it)
                }
                val desiredNce = id == "kirby-star-allies-26-2" || id == "metal-dogs-stable"
                if (settings.optBoolean("preferNce", !desiredNce) != desiredNce) {
                    settings.put("preferNce", desiredNce)
                    changed = true
                }
                val description = item.optString("description")
                if (id == "kirby-star-allies-26-2" && ("Dynarmic" in description || "强制关闭 NCE" in description)) {
                    item.put(
                        "description",
                        "优先 Turnip 26.2，避开 T25 蓝块和 26.3 黑屏；NCE + alternate signal stack + 1X + 图形兼容。"
                    )
                    changed = true
                } else if (id == "metal-dogs-stable" && "Dynarmic" in description) {
                    item.put(
                        "description",
                        "26.2/全局稳定驱动 + NCE + 1X + 图形兼容，避免高性能 profile 的 renderStall。"
                    )
                    changed = true
                }
            }
            if (changed) {
                file.writeText(root.toString(2), Charsets.UTF_8)
            }
        }
    }

    fun findPreset(context: Context, gameName: String, gamePath: String): Preset? {
        val file = ensureProfileFile(context)
        val text = runCatching { file.readText(Charsets.UTF_8) }.getOrNull() ?: return null
        val root = runCatching { JSONObject(text) }.getOrNull() ?: return null
        val profiles = root.optJSONArray("profiles") ?: return null
        val key = "$gameName\n$gamePath".lowercase(Locale.US)
        val titleId = extractTitleId(key).orEmpty().lowercase(Locale.US)
        for (i in 0 until profiles.length()) {
            val item = profiles.optJSONObject(i) ?: continue
            if (!item.optBoolean("enabled", true)) continue
            if (!matches(item, key, titleId)) continue
            return Preset(
                id = item.optString("id").ifBlank { "profile-$i" },
                title = item.optString("title").ifBlank { item.optString("id").ifBlank { "推荐配置" } },
                description = item.optString("description").ifBlank { "来自 $PROFILE_FILE_NAME" },
                profile = parseProfile(context, item.optJSONObject("settings") ?: JSONObject()),
                source = file.absolutePath
            )
        }
        return null
    }

    fun listPresets(context: Context): List<Preset> {
        val file = ensureProfileFile(context)
        val text = runCatching { file.readText(Charsets.UTF_8) }.getOrNull() ?: return emptyList()
        val root = runCatching { JSONObject(text) }.getOrNull() ?: return emptyList()
        val profiles = root.optJSONArray("profiles") ?: return emptyList()
        val result = mutableListOf<Preset>()
        for (i in 0 until profiles.length()) {
            val item = profiles.optJSONObject(i) ?: continue
            val settings = item.optJSONObject("settings") ?: JSONObject()
            result += Preset(
                id = item.optString("id").ifBlank { "profile-$i" },
                title = item.optString("title").ifBlank { item.optString("id").ifBlank { "推荐配置" } },
                description = item.optString("description").ifBlank { "来自 $PROFILE_FILE_NAME" },
                profile = parseProfile(context, settings),
                source = file.absolutePath
            )
        }
        return result
    }

    fun validateJson(text: String): String? {
        val root = runCatching { JSONObject(text) }.getOrElse { return "JSON 格式错误：${it.message}" }
        val profiles = root.optJSONArray("profiles") ?: return "缺少 profiles 数组"
        for (i in 0 until profiles.length()) {
            val item = profiles.optJSONObject(i) ?: return "profiles[$i] 不是对象"
            if (!item.has("id")) return "profiles[$i] 缺少 id"
            val settings = item.optJSONObject("settings") ?: return "profiles[$i] 缺少 settings"
            if (settings.has("frameSkip") && settings.optInt("frameSkip", 0) !in 0..4) {
                return "profiles[$i].settings.frameSkip 必须是 0..4"
            }
        }
        return null
    }

    fun savePresetForGame(
        context: Context,
        gameName: String,
        gamePath: String,
        profile: AppPreferences.GameProfile
    ): String {
        val file = ensureProfileFile(context)
        val root = runCatching { JSONObject(file.readText(Charsets.UTF_8)) }
            .getOrElse { JSONObject(defaultJson()) }
        val profiles = root.optJSONArray("profiles") ?: JSONArray().also { root.put("profiles", it) }
        val titleId = extractTitleId("$gameName\n$gamePath")
        val id = buildPresetId(gameName, titleId)
        val item = JSONObject().apply {
            put("id", id)
            put("enabled", true)
            put("title", "${gameName.ifBlank { id }} 自定义配置")
            put("description", "从 NxEmu Android 游戏属性页保存。")
            put("titleIds", JSONArray().apply { if (!titleId.isNullOrBlank()) put(titleId) })
            put("nameContains", JSONArray().apply {
                val token = gameName.substringBeforeLast('.').trim()
                if (token.isNotBlank()) put(token)
            })
            put("settings", profileToJson(profile))
        }

        var replaced = false
        for (i in 0 until profiles.length()) {
            val old = profiles.optJSONObject(i) ?: continue
            if (old.optString("id") == id) {
                profiles.put(i, item)
                replaced = true
                break
            }
        }
        if (!replaced) {
            profiles.put(item)
        }
        file.writeText(root.toString(2), Charsets.UTF_8)
        return "profileSave=${if (replaced) "replaced" else "added"}\nid=$id\npath=${file.absolutePath}"
    }

    fun deletePreset(context: Context, id: String): String {
        val file = ensureProfileFile(context)
        val root = runCatching { JSONObject(file.readText(Charsets.UTF_8)) }
            .getOrElse { return "profileDelete=failed\nreason=${it.message}" }
        val profiles = root.optJSONArray("profiles") ?: return "profileDelete=failed\nreason=no profiles"
        val next = JSONArray()
        var deleted = false
        for (i in 0 until profiles.length()) {
            val item = profiles.optJSONObject(i)
            if (item != null && item.optString("id") == id) {
                deleted = true
            } else {
                next.put(profiles.get(i))
            }
        }
        root.put("profiles", next)
        file.writeText(root.toString(2), Charsets.UTF_8)
        return "profileDelete=${if (deleted) "ok" else "not-found"}\nid=$id\npath=${file.absolutePath}"
    }

    fun statusText(context: Context): String {
        val file = ensureProfileFile(context)
        val count = runCatching {
            JSONObject(file.readText(Charsets.UTF_8)).optJSONArray("profiles")?.length() ?: 0
        }.getOrDefault(0)
        return "profileCatalog=$count profiles\nprofileCatalogPath=${file.absolutePath}"
    }

    private fun matches(item: JSONObject, key: String, titleId: String): Boolean {
        val titleIds = item.optJSONArray("titleIds") ?: JSONArray()
        for (i in 0 until titleIds.length()) {
            if (titleIds.optString(i).lowercase(Locale.US) == titleId && titleId.isNotBlank()) {
                return true
            }
        }
        val contains = item.optJSONArray("nameContains") ?: JSONArray()
        for (i in 0 until contains.length()) {
            val token = contains.optString(i).lowercase(Locale.US)
            if (token.isNotBlank() && token in key) return true
        }
        return false
    }

    private fun parseProfile(context: Context, settings: JSONObject): AppPreferences.GameProfile {
        val global = AppPreferences.globalGameProfile(context)
        return AppPreferences.GameProfile(
            enabled = true,
            frameSkip = settings.optInt("frameSkip", global.frameSkip).coerceIn(0, 4),
            resolutionSetup = parseResolution(settings.optString("resolution", ""), settings.optInt("resolutionSetup", global.resolutionSetup)),
            aspectRatio = parseAspect(settings.optString("aspect", ""), settings.optInt("aspectRatio", global.aspectRatio)),
            graphicsCompat = settings.optBoolean("graphicsCompat", global.graphicsCompat),
            preferNce = settings.optBoolean("preferNce", global.preferNce),
            perfHudDetailed = settings.optBoolean("perfHudDetailed", global.perfHudDetailed),
            autoOutputLog = settings.optBoolean("autoOutputLog", global.autoOutputLog),
            driverSource = resolveDriverHint(settings.optString("driverHint").ifBlank {
                settings.optString("driverSource")
            })
        )
    }

    private fun parseResolution(label: String, fallback: Int): Int = when (label.lowercase(Locale.US)) {
        "1/2x", "0.5x", "half" -> 0
        "3/4x", "0.75x" -> 1
        "1x", "native" -> 2
        else -> fallback
    }.coerceIn(0, 2)

    private fun parseAspect(label: String, fallback: Int): Int = when (label.lowercase(Locale.US)) {
        "16:9" -> 0
        "4:3" -> 1
        "21:9" -> 2
        "16:10" -> 3
        "stretch", "拉伸", "拉伸全屏" -> 4
        else -> fallback
    }.coerceIn(0, 4)

    private fun resolveDriverHint(hint: String): String {
        val lower = hint.lowercase(Locale.US)
        if (lower.isBlank() || lower == "global" || lower == "default") return ""
        if (File(hint).exists()) return hint
        return runCatching {
            GpuDriverHelper.listDriverChoices()
                .firstOrNull { choice ->
                    val text = "${choice.label}\n${choice.detail}\n${choice.source}".lowercase(Locale.US)
                    when {
                        "turnip26.2" in lower || "26.2" in lower || "v306" in lower || "b860e01" in lower ->
                            "26.2" in text || "v306" in text || "b860e01" in text
                        "t25" in lower || "purple" in lower -> "t25" in text || "purple" in text
                        else -> lower in text
                    }
                }?.source.orEmpty()
        }.getOrDefault("")
    }

    private fun extractTitleId(text: String): String? =
        Regex("""0100[0-9a-fA-F]{12}""").find(text)?.value?.uppercase(Locale.US)

    private fun buildPresetId(gameName: String, titleId: String?): String {
        val base = titleId?.lowercase(Locale.US)
            ?: gameName.substringBeforeLast('.')
                .lowercase(Locale.US)
                .replace(Regex("""[^a-z0-9]+"""), "-")
                .trim('-')
                .ifBlank { "custom-game" }
        return "$base-custom"
    }

    private fun profileToJson(profile: AppPreferences.GameProfile): JSONObject =
        JSONObject().apply {
            put("frameSkip", profile.frameSkip)
            put("resolutionSetup", profile.resolutionSetup)
            put("resolution", AppPreferences.resolutionLabel(profile.resolutionSetup))
            put("aspectRatio", profile.aspectRatio)
            put("aspect", AppPreferences.aspectLabel(profile.aspectRatio))
            put("graphicsCompat", profile.graphicsCompat)
            put("preferNce", profile.preferNce)
            put("perfHudDetailed", profile.perfHudDetailed)
            put("autoOutputLog", profile.autoOutputLog)
            if (profile.driverSource.isNotBlank()) {
                put("driverSource", profile.driverSource)
            }
        }

    fun defaultJson(): String = """
{
  "version": 1,
  "notes": "NxEmu Android per-game recommendation profiles. You can edit this file and restart the app.",
  "profiles": [
    {
      "id": "metal-dogs-stable",
      "enabled": true,
      "title": "Metal Dogs 稳定加载配置",
      "description": "26.2/全局稳定驱动 + NCE + 1X + 图形兼容，避免高性能 profile 的 renderStall。",
      "titleIds": ["0100A6E01681C000"],
      "nameContains": ["metal dogs"],
      "settings": {
        "frameSkip": 0,
        "resolution": "1X",
        "aspect": "stretch",
        "graphicsCompat": true,
        "preferNce": true,
        "perfHudDetailed": false,
        "autoOutputLog": false,
        "driverHint": "turnip26.2"
      }
    },
    {
      "id": "kirby-star-allies-26-2",
      "enabled": true,
      "title": "Kirby Star Allies 26.2 正常显示配置",
      "description": "优先 Turnip 26.2，避开 T25 蓝块和 26.3 黑屏；NCE + alternate signal stack + 1X + 图形兼容。",
      "titleIds": [],
      "nameContains": ["kirby", "星之卡比"],
      "settings": {
        "frameSkip": 0,
        "resolution": "1X",
        "aspect": "stretch",
        "graphicsCompat": true,
        "preferNce": true,
        "perfHudDetailed": false,
        "autoOutputLog": false,
        "driverHint": "turnip26.2"
      }
    }
  ]
}
""".trimIndent()
}
