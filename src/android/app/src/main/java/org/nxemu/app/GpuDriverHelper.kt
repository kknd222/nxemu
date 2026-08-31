package org.nxemu.app

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.File
import java.util.Locale
import java.util.zip.ZipException
import java.util.zip.ZipFile

object GpuDriverHelper {
    private const val META_JSON_FILENAME = "meta.json"
    private const val MAX_META_SIZE_BYTES = 500_000L

    private lateinit var appContext: Context

    private val driverStorageDir: File
        get() = File(appContext.filesDir, "gpu_drivers")
    private val driverInstallDir: File
        get() = File(appContext.filesDir, "gpu_driver/current")
    private val fileRedirectDir: File
        get() = File(appContext.getExternalFilesDir(null), "gpu/vk_file_redirect")
    private val selectedMarker: File
        get() = File(appContext.filesDir, "gpu_driver/selected_zip.txt")

    fun initialize(context: Context): String {
        appContext = context.applicationContext
        ensureDirectories()
        val restoreStatus = restoreSelectedDriverIfNeeded()
        val installed = installedDriverMetadata()
        val nativeStatus = NativeLibrary.initializeGpuDriver(
            hookLibDir = appContext.applicationInfo.nativeLibraryDir + "/",
            customDriverDir = driverInstallDir.absolutePath + "/",
            customDriverName = installed.libraryName.orEmpty(),
            fileRedirectDir = fileRedirectDir.absolutePath + "/"
        )
        return buildString {
            if (restoreStatus.isNotBlank()) {
                appendLine(restoreStatus)
            }
            append(nativeStatus)
        }
    }

    fun supportsCustomDriverLoading(): Boolean {
        return runCatching { NativeLibrary.supportsCustomDriverLoading() }.getOrDefault(false)
    }

    fun hasAllFilesAccess(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()
    }

    fun summaryText(): String = buildString {
        ensureDirectories()
        val installed = installedDriverMetadata()
        val selected = selectedMarker.takeIf { it.exists() }?.readText()?.trim().orEmpty()
        appendLine("lastDriver=${installed.name ?: "system/default"}")
        appendLine("lastDriverVersion=${installed.version.orEmpty()}")
        appendLine("lastDriverLibrary=${installed.libraryName.orEmpty()}")
        appendLine("lastDriverSource=${selected.ifBlank { "system/default" }}")
    }

    fun currentSelectedDriverSource(): String {
        ensureDirectories()
        return selectedMarker.takeIf { it.exists() }?.readText()?.trim().orEmpty()
    }

    fun useDriverSource(source: String): String {
        ensureDirectories()
        val trimmed = source.trim()
        if (trimmed.isBlank() || trimmed == "system/default") {
            return installDefaultDriver()
        }
        val file = File(trimmed)
        if (!file.exists()) {
            return "driverPerGame=missing\nsource=$trimmed"
        }
        return installDriverPath(file, "per-game-profile")
    }

    fun listDriverChoices(): List<DriverChoice> {
        ensureDirectories()
        val current = currentSelectedDriverSource()
        val choices = mutableListOf(
            DriverChoice(
                label = "跟随全局/系统默认",
                source = "",
                detail = if (current.isBlank()) "当前全局：系统 Vulkan 驱动" else "当前全局：${File(current).name}"
            )
        )
        listStoredDrivers().forEach { entry ->
            val version = entry.metadata.version.orEmpty()
            choices += DriverChoice(
                label = entry.metadata.name ?: entry.file.name,
                source = entry.file.absolutePath,
                detail = buildString {
                    append(entry.file.name)
                    if (version.isNotBlank()) append(" · $version")
                    entry.metadata.libraryName?.let { append(" · $it") }
                }
            )
        }
        listCandidateFilesFromDefaultNsFolder().take(12).forEach { file ->
            if (choices.none { it.source == file.absolutePath }) {
                val metadata = if (file.isDirectory) metadataFromDirectory(file) else metadataFromZip(file)
                choices += DriverChoice(
                    label = metadata.name ?: file.name,
                    source = file.absolutePath,
                    detail = buildString {
                        append(file.absolutePath)
                        metadata.version?.let { append(" · $it") }
                    }
                )
            }
        }
        return choices.distinctBy { it.source }
    }

    fun statusText(): String = buildString {
        ensureDirectories()
        val installed = installedDriverMetadata()
        appendLine("GPU driver:")
        appendLine("supportsCustomDriverLoading=${supportsCustomDriverLoading()}")
        appendLine("externalStorageManager=${hasAllFilesAccess()}")
        appendLine("defaultDriverFolderReadable=${listCandidateFilesFromDefaultNsFolder().isNotEmpty()}")
        appendLine("driverStorageDir=${driverStorageDir.absolutePath}")
        appendLine("driverInstallDir=${driverInstallDir.absolutePath}")
        appendLine("fileRedirectDir=${fileRedirectDir.absolutePath}")
        appendLine("fileRedirectSize=${fileRedirectDir.sizeBytesRecursive()} bytes")
        appendLine("installedName=${installed.name ?: "system/default"}")
        appendLine("installedVersion=${installed.version.orEmpty()}")
        appendLine("installedLibrary=${installed.libraryName.orEmpty()}")
        appendLine("selectedDriver=${selectedMarker.takeIf { it.exists() }?.readText().orEmpty()}")
        appendLine("restoreStatus=${restoreSelectedDriverIfNeeded()}")
        appendLine("storedDriverZips=${listStoredDrivers().size}")
        listStoredDrivers().take(8).forEachIndexed { index, entry ->
            appendLine("[$index] ${entry.file.name}")
            appendLine("    name=${entry.metadata.name.orEmpty()}")
            appendLine("    version=${entry.metadata.version.orEmpty()}")
            appendLine("    library=${entry.metadata.libraryName.orEmpty()}")
        }
    }

    fun clearGraphicsCaches(): String {
        ensureDirectories()
        val candidates = listOf(
            CacheTarget("adrenotoolsFileRedirect", fileRedirectDir),
            CacheTarget("appCache", appContext.cacheDir),
            CacheTarget("externalCache", appContext.externalCacheDir),
            // nxemu/yuzu-style shader caches are created below files/user/shader
            // (for example files/user/shader/<titleid>/vulkan_pipelines.bin).  The
            // older cleaner only removed files/shader and therefore left the real
            // per-title Vulkan pipeline cache intact; that made driver/compat
            // comparisons for Kirby black/blue blocks misleading.
            CacheTarget("userShaderCache", File(appContext.filesDir, "user/shader")),
            CacheTarget("userPipelineCache", File(appContext.filesDir, "user/pipeline_cache")),
            CacheTarget("userVulkanCache", File(appContext.filesDir, "user/vulkan_cache")),
            CacheTarget("userCache", File(appContext.filesDir, "user/cache")),
            CacheTarget("shaderCache", File(appContext.filesDir, "shader")),
            CacheTarget("shaderCaches", File(appContext.filesDir, "shader_caches")),
            CacheTarget("pipelineCache", File(appContext.filesDir, "pipeline_cache")),
            CacheTarget("vulkanCache", File(appContext.filesDir, "vulkan_cache")),
            CacheTarget("gpuCache", File(appContext.filesDir, "gpu/cache")),
            CacheTarget("gpuShaderCache", File(appContext.getExternalFilesDir(null), "gpu/shader_cache")),
            CacheTarget("gpuPipelineCache", File(appContext.getExternalFilesDir(null), "gpu/pipeline_cache"))
        ).filter { it.dir != null }

        return buildString {
            appendLine("clearGraphicsCaches=begin")
            appendLine("hint=如果 Kirby 黑色方块清理后消失，更偏向 shader/driver cache 污染；如果仍存在，更偏向驱动或渲染兼容 bug。")
            candidates.distinctBy { it.dir!!.absolutePath }.forEach { target ->
                val dir = target.dir ?: return@forEach
                val before = dir.sizeBytesRecursive()
                val existed = dir.exists()
                val deleted = runCatching {
                    dir.deleteRecursively()
                    dir.mkdirs()
                    true
                }.getOrDefault(false)
                val after = dir.sizeBytesRecursive()
                appendLine("${target.name}: existed=$existed deleted=$deleted before=$before after=$after path=${dir.absolutePath}")
            }
            appendLine("driverAfterClear:")
            append(summaryText())
            append("clearGraphicsCaches=end")
        }
    }

    fun listCandidateFilesFromDefaultNsFolder(): List<File> {
        val roots = listOf(
            File("/sdcard/ns/qudong/new"),
            File("/sdcard/ns/qudong")
        )
        return roots.flatMap { root -> scanDriverCandidates(root, maxDepth = 3) }
            .distinctBy { it.absolutePath }
            .sortedByDescending { driverPreferenceScore(it.name) }
    }

    fun installBestDefaultNsDriver(): String {
        val candidates = listCandidateFilesFromDefaultNsFolder()
        if (candidates.isEmpty()) {
            return buildString {
                appendLine("driverInstall=failed")
                appendLine("reason=/sdcard/ns/qudong 未发现可直接读取的 zip")
                appendLine("externalStorageManager=${hasAllFilesAccess()}")
                appendLine("hint=Android 11+ 如需直接扫描 /sdcard/ns/qudong，请先点“授权所有文件访问”；否则用“选择驱动ZIP”或“授权驱动目录”走 SAF。")
            }
        }
        return installDriverPath(candidates.first(), "default-ns-folder")
    }

    fun installDriverFromUri(uri: Uri): String {
        ensureDirectories()
        val name = queryDisplayName(uri).ifBlank { uri.lastPathSegment.orEmpty() }
            .substringAfterLast('/')
            .ifBlank { "driver.zip" }
        val copied = File(driverStorageDir, sanitizeFilename(name))
        appContext.contentResolver.openInputStream(uri)?.use { input ->
            copied.outputStream().use { output -> input.copyTo(output) }
        } ?: return "driverInstall=failed\nreason=openInputStream failed\nuri=$uri"
        return installDriverPath(copied, "saf-uri")
    }

    fun scanDriverFolder(uri: Uri, maxDepth: Int = 2): List<Uri> {
        return SafScanner.scanDocuments(appContext.contentResolver, uri, maxDepth) { name, mime ->
            mime != android.provider.DocumentsContract.Document.MIME_TYPE_DIR &&
                name.endsWith(".zip", ignoreCase = true)
        }
    }

    fun installFirstDriverFromFolder(uri: Uri): String {
        val drivers = scanDriverFolder(uri)
        if (drivers.isEmpty()) {
            return "driverInstall=failed\nreason=授权目录里没有 zip 驱动\nuri=$uri"
        }
        val preferred = drivers.sortedByDescending { driverPreferenceScore(it.toString()) }.first()
        return installDriverFromUri(preferred)
    }

    private fun driverPreferenceScore(text: String): Int {
        val lower = text.lowercase(Locale.US)
        var score = 0
        if ("turnip" in lower) score += 1000
        // RMX3700/Adreno 7xx comparison on 2026-08-16:
        // - Turnip-v26.2.0-20260418.zip / v306-b860e01: Kirby title/save scene renders normally
        //   in A/B testing; this is the current preferred default for RMX3700/Adreno 7xx.
        // - turnip_mrpurple_T25-raw.adpkg.zip: Kirby can enter gameplay but leaves deep-blue
        //   blocks/water traces until geometry redraws affected areas.
        // - Turnip-v26.3.0-devel.zip: renders black screen with FPS.
        // - mesa-turnip-main-V26.1-eden-fix and Biosensor MK-I: large blue block artifacts.
        // Prefer 26.2 as the current safe default instead of blindly picking
        // the newest 26.3 package.
        if ("26.2" in lower || "v306" in lower || "b860e01" in lower) score += 2200
        if ("t25" in lower || "purple" in lower || "mrpurple" in lower) score += 900
        if ("710" in lower || "720" in lower || "722" in lower || "725" in lower || "7xx" in lower) score += 450
        if ("eden" in lower || "crash-fix" in lower || "fix" in lower) score -= 180
        if ("26.3" in lower) score += 300
        if ("vulkan 1.4" in lower) score += 120
        if ("26.1" in lower) score += 760
        if ("26.0" in lower) score += 620
        if ("25.1.9" in lower) score += 540
        if ("v849" in lower) score += 700
        if ("a8xx" in lower) score -= 700
        if ("biosensor" in lower || "mk-i" in lower || "mkii" in lower || "mkii" in lower) score -= 700
        if ("mtr" in lower) score -= 120
        if (lower.endsWith(".zip")) score += 20
        return score
    }

    fun installDefaultDriver(): String {
        driverInstallDir.deleteRecursively()
        selectedMarker.delete()
        ensureDirectories()
        return initialize(appContext)
    }

    private fun installDriverPath(driverPath: File, source: String): String {
        return if (driverPath.isDirectory) {
            installDriverDirectory(driverPath, source)
        } else {
            installDriverZip(driverPath, source)
        }
    }

    private fun restoreSelectedDriverIfNeeded(): String {
        ensureDirectories()
        val installed = installedDriverMetadata()
        if (!installed.libraryName.isNullOrBlank() && File(driverInstallDir, META_JSON_FILENAME).exists()) {
            return "driverRestore=current-installed\nselected=${selectedMarker.takeIf { it.exists() }?.readText().orEmpty()}"
        }

        val selectedPath = selectedMarker.takeIf { it.exists() }?.readText()?.trim().orEmpty()
        if (selectedPath.isBlank()) {
            return "driverRestore=system-default"
        }

        val selected = File(selectedPath)
        if (!selected.exists()) {
            return "driverRestore=missing-selected\nselected=$selectedPath"
        }

        return restoreDriverPath(selected)
    }

    private fun restoreDriverPath(driverPath: File): String {
        return runCatching {
            val metadata = if (driverPath.isDirectory) {
                metadataFromDirectory(driverPath)
            } else {
                metadataFromZip(driverPath)
            }
            if (metadata.name == null || metadata.libraryName.isNullOrBlank()) {
                return "driverRestore=failed\nreason=meta.json/libraryName invalid\nselected=${driverPath.absolutePath}"
            }
            if (metadata.minApi > Build.VERSION.SDK_INT) {
                return "driverRestore=failed\nreason=minApi ${metadata.minApi} > sdk ${Build.VERSION.SDK_INT}\nselected=${driverPath.absolutePath}"
            }

            driverInstallDir.deleteRecursively()
            driverInstallDir.mkdirs()
            if (driverPath.isDirectory) {
                copyDirectory(driverPath, driverInstallDir)
            } else {
                unzip(driverPath, driverInstallDir)
            }
            buildString {
                appendLine("driverRestore=ok")
                appendLine("selected=${driverPath.absolutePath}")
                appendLine("name=${metadata.name}")
                appendLine("version=${metadata.version.orEmpty()}")
                append("library=${metadata.libraryName}")
            }
        }.getOrElse { error ->
            "driverRestore=exception\nselected=${driverPath.absolutePath}\n${error.stackTraceToString()}"
        }
    }

    private fun installDriverDirectory(driverDir: File, source: String): String {
        ensureDirectories()
        val metadata = metadataFromDirectory(driverDir)
        if (metadata.name == null || metadata.libraryName.isNullOrBlank()) {
            return "driverInstall=failed\nreason=meta.json/libraryName invalid\nsource=$source\ndir=${driverDir.absolutePath}"
        }
        if (metadata.minApi > Build.VERSION.SDK_INT) {
            return "driverInstall=failed\nreason=minApi ${metadata.minApi} > sdk ${Build.VERSION.SDK_INT}\ndir=${driverDir.absolutePath}"
        }

        driverInstallDir.deleteRecursively()
        driverInstallDir.mkdirs()
        copyDirectory(driverDir, driverInstallDir)
        selectedMarker.parentFile?.mkdirs()
        selectedMarker.writeText(driverDir.absolutePath)

        val init = initialize(appContext)
        return buildString {
            appendLine("driverInstall=ok")
            appendLine("source=$source")
            appendLine("dir=${driverDir.absolutePath}")
            appendLine("name=${metadata.name}")
            appendLine("version=${metadata.version.orEmpty()}")
            appendLine("library=${metadata.libraryName}")
            append(init)
        }
    }

    private fun installDriverZip(driverZip: File, source: String): String {
        ensureDirectories()
        val metadata = metadataFromZip(driverZip)
        if (metadata.name == null || metadata.libraryName.isNullOrBlank()) {
            return "driverInstall=failed\nreason=meta.json/libraryName invalid\nsource=$source\nzip=${driverZip.absolutePath}"
        }
        if (metadata.minApi > Build.VERSION.SDK_INT) {
            return "driverInstall=failed\nreason=minApi ${metadata.minApi} > sdk ${Build.VERSION.SDK_INT}\nzip=${driverZip.absolutePath}"
        }

        driverInstallDir.deleteRecursively()
        driverInstallDir.mkdirs()
        unzip(driverZip, driverInstallDir)
        selectedMarker.parentFile?.mkdirs()
        selectedMarker.writeText(driverZip.absolutePath)

        val init = initialize(appContext)
        return buildString {
            appendLine("driverInstall=ok")
            appendLine("source=$source")
            appendLine("zip=${driverZip.absolutePath}")
            appendLine("name=${metadata.name}")
            appendLine("version=${metadata.version.orEmpty()}")
            appendLine("library=${metadata.libraryName}")
            append(init)
        }
    }

    private fun listStoredDrivers(): List<DriverEntry> {
        ensureDirectories()
        return driverStorageDir.listFiles()
            ?.filter { it.isFile && it.extension.lowercase(Locale.US) == "zip" }
            ?.map { DriverEntry(it, metadataFromZip(it)) }
            ?.filter { it.metadata.name != null }
            ?.sortedByDescending { it.metadata.name }
            .orEmpty()
    }

    private fun installedDriverMetadata(): GpuDriverMetadata {
        val meta = File(driverInstallDir, META_JSON_FILENAME)
        return if (meta.exists()) metadataFromJson(meta.readText()) else GpuDriverMetadata()
    }

    private fun metadataFromZip(driver: File): GpuDriverMetadata {
        if (!driver.exists()) {
            return GpuDriverMetadata()
        }
        return try {
            ZipFile(driver).use { zf ->
                val entries = zf.entries()
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (!entry.isDirectory && entry.name.lowercase(Locale.US).endsWith(".json") &&
                        entry.size in 1..MAX_META_SIZE_BYTES
                    ) {
                        zf.getInputStream(entry).use { input ->
                            return metadataFromJson(input.bufferedReader().readText())
                        }
                    }
                }
                GpuDriverMetadata()
            }
        } catch (_: ZipException) {
            GpuDriverMetadata()
        }
    }

    private fun metadataFromDirectory(driverDir: File): GpuDriverMetadata {
        if (!driverDir.exists() || !driverDir.isDirectory) {
            return GpuDriverMetadata()
        }
        val meta = driverDir.walkTopDown()
            .maxDepth(3)
            .firstOrNull { it.isFile && it.name.equals(META_JSON_FILENAME, ignoreCase = true) }
            ?: return GpuDriverMetadata()
        return metadataFromJson(meta.readText())
    }

    private fun metadataFromJson(text: String): GpuDriverMetadata {
        return runCatching {
            val json = JSONObject(text)
            GpuDriverMetadata(
                name = json.optString("name").ifBlank { null },
                description = json.optString("description").ifBlank { null },
                author = json.optString("author").ifBlank { null },
                vendor = json.optString("vendor").ifBlank { null },
                version = json.optString("driverVersion").ifBlank { null },
                minApi = json.optInt("minApi", 0),
                libraryName = json.optString("libraryName").ifBlank { null }
            )
        }.getOrDefault(GpuDriverMetadata())
    }

    private fun unzip(zip: File, destination: File) {
        ZipFile(zip).use { zf ->
            val canonicalDestination = destination.canonicalFile
            val entries = zf.entries()
            while (entries.hasMoreElements()) {
                val entry = entries.nextElement()
                val out = File(destination, entry.name).canonicalFile
                require(out.path.startsWith(canonicalDestination.path)) {
                    "zip entry escapes destination: ${entry.name}"
                }
                if (entry.isDirectory) {
                    out.mkdirs()
                } else {
                    out.parentFile?.mkdirs()
                    zf.getInputStream(entry).use { input ->
                        out.outputStream().use { output -> input.copyTo(output) }
                    }
                }
            }
        }
    }

    private fun copyDirectory(source: File, destination: File) {
        val canonicalSource = source.canonicalFile
        val canonicalDestination = destination.canonicalFile
        source.walkTopDown().forEach { entry ->
            val relative = entry.canonicalFile.relativeTo(canonicalSource)
            val out = File(canonicalDestination, relative.path).canonicalFile
            require(out.path.startsWith(canonicalDestination.path)) {
                "driver entry escapes destination: ${entry.absolutePath}"
            }
            if (entry.isDirectory) {
                out.mkdirs()
            } else {
                out.parentFile?.mkdirs()
                entry.inputStream().use { input ->
                    out.outputStream().use { output -> input.copyTo(output) }
                }
            }
        }
    }

    private fun scanDriverCandidates(root: File, maxDepth: Int): List<File> {
        if (!root.exists() || !root.canRead()) {
            return emptyList()
        }
        val results = mutableListOf<File>()
        fun scan(dir: File, depth: Int) {
            if (depth < 0 || !dir.isDirectory || !dir.canRead()) {
                return
            }
            val children = dir.listFiles().orEmpty()
            val hasMeta = children.any { it.isFile && it.name.equals(META_JSON_FILENAME, ignoreCase = true) }
            val hasVulkanSo = children.any { it.isFile && it.name.startsWith("libvulkan", ignoreCase = true) && it.name.endsWith(".so", ignoreCase = true) }
            if (hasMeta && hasVulkanSo) {
                results += dir
            }
            children.forEach { child ->
                if (child.isFile && child.extension.lowercase(Locale.US) == "zip") {
                    results += child
                } else if (child.isDirectory) {
                    scan(child, depth - 1)
                }
            }
        }
        if (root.isFile && root.extension.lowercase(Locale.US) == "zip") {
            return listOf(root)
        }
        scan(root, maxDepth)
        return results
    }

    private fun queryDisplayName(uri: Uri): String {
        appContext.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            .use { cursor ->
                if (cursor != null && cursor.moveToFirst()) {
                    val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (index >= 0) {
                        return cursor.getString(index).orEmpty()
                    }
                }
            }
        return uri.lastPathSegment.orEmpty()
    }

    private fun ensureDirectories() {
        driverStorageDir.mkdirs()
        driverInstallDir.mkdirs()
        fileRedirectDir.mkdirs()
    }

    private fun File?.sizeBytesRecursive(): Long {
        val root = this ?: return 0L
        if (!root.exists()) return 0L
        return runCatching {
            if (root.isFile) root.length() else root.walkTopDown().filter { it.isFile }.sumOf { it.length() }
        }.getOrDefault(0L)
    }

    private fun sanitizeFilename(name: String): String {
        return name.replace(Regex("""[\\/:*?"<>|]"""), "_")
    }

    data class GpuDriverMetadata(
        val name: String? = null,
        val description: String? = null,
        val author: String? = null,
        val vendor: String? = null,
        val version: String? = null,
        val minApi: Int = 0,
        val libraryName: String? = null
    )

    data class DriverChoice(
        val label: String,
        val source: String,
        val detail: String
    )

    private data class DriverEntry(
        val file: File,
        val metadata: GpuDriverMetadata
    )

    private data class CacheTarget(
        val name: String,
        val dir: File?
    )
}

