package org.nxemu.app

import android.content.Context
import android.net.Uri
import java.io.File
import java.net.URLDecoder
import java.util.Locale

object GamePathResolver {
    private val supportedExtensions = setOf("dxci", "dnsp", "xci", "nsp", "nro", "nca")

    fun isSupportedGameName(name: String): Boolean {
        val ext = name.substringAfterLast('.', missingDelimiterValue = "").lowercase(Locale.US)
        return ext in supportedExtensions
    }

    fun normalize(raw: String?): String {
        var path = raw.orEmpty().trim()
        if ((path.startsWith("\"") && path.endsWith("\"")) ||
            (path.startsWith("'") && path.endsWith("'"))) {
            path = path.substring(1, path.length - 1)
        }
        val isUri = path.contains("://")
        if (path.startsWith("file://", ignoreCase = true)) {
            path = Uri.parse(path).path.orEmpty()
        }
        if (isUri || path.contains('%')) {
            return runCatching { URLDecoder.decode(path, "UTF-8") }.getOrDefault(path)
        }
        return path
    }

    fun resolve(context: Context, raw: String?): Result {
        val normalized = normalize(raw)
        if (normalized.isBlank()) {
            return Result("", normalized, false, "blank")
        }
        externalStorageDocumentPath(normalized)?.let { path ->
            if (existsAsHostPath(path)) {
                return Result(path, normalized, true, "content-primary-document")
            }
        }
        if (existsAsHostPath(normalized)) {
            return Result(normalized, normalized, false, "exists")
        }

        findByPrefix(context, normalized)?.let { fixed ->
            return Result(fixed.absolutePath, normalized, true, "prefix-scan")
        }

        return Result(normalized, normalized, false, "not-found")
    }

    fun defaultRomRoots(context: Context): List<File> {
        val roots = linkedSetOf<File>()
        roots += File("/sdcard/ns/rom")
        roots += File("/sdcard/ns/roms")
        roots += File("/storage/emulated/0/ns/rom")
        roots += File("/storage/emulated/0/ns/roms")
        val last = AppPreferences.lastGameFolder(context).ifBlank { null }
        if (last != null) {
            roots += File(last)
        }
        val lastGame = normalize(AppPreferences.lastGamePath(context))
        if (lastGame.isNotBlank() && !lastGame.startsWith("content://", ignoreCase = true)) {
            File(lastGame).parentFile?.let { roots += it }
        }
        return roots.toList()
    }

    private fun existsAsHostPath(path: String): Boolean {
        if (path.startsWith("content://", ignoreCase = true)) {
            return false
        }
        return File(path).isFile
    }

    private fun externalStorageDocumentPath(normalized: String): String? {
        if (!normalized.startsWith("content://com.android.externalstorage.documents/", ignoreCase = true)) {
            return null
        }
        val documentId = normalized.substringAfter("/document/", missingDelimiterValue = "")
            .substringBefore('?')
            .substringBefore('#')
        val primaryRelative = when {
            documentId.startsWith("primary:", ignoreCase = true) -> documentId.substringAfter(':')
            normalized.contains("primary:", ignoreCase = true) -> normalized.substringAfter("primary:")
            else -> return null
        }.trimStart('/')
        if (primaryRelative.isBlank()) {
            return null
        }

        val candidates = listOf(
            File("/sdcard", primaryRelative),
            File("/storage/emulated/0", primaryRelative),
        )
        return candidates.firstOrNull { it.isFile }?.absolutePath ?: candidates.first().absolutePath
    }

    private fun findByPrefix(context: Context, normalized: String): File? {
        val target = File(normalized)
        val rawName = target.name
        if (rawName.isBlank()) {
            return null
        }
        val lowerName = rawName.lowercase(Locale.US)
        val requestedExt = rawName.substringAfterLast('.', missingDelimiterValue = "").lowercase(Locale.US)
        val requestedHasPreferredExt = requestedExt in setOf("dnsp", "dxci", "nro", "nca")
        val parent = target.parentFile
        val roots = buildList {
            if (parent != null) add(parent)
            addAll(defaultRomRoots(context))
        }.distinctBy { it.absolutePath }

        val matches = roots
            .asSequence()
            .filter { it.exists() && it.canRead() }
            .flatMap { root ->
                runCatching { root.walkTopDown().maxDepth(3).asSequence() }
                    .getOrElse { emptySequence() }
            }
            .filter { it.isFile && isSupportedGameName(it.name) }
            .filter { file ->
                val candidate = file.name.lowercase(Locale.US)
                candidate == lowerName ||
                    candidate.startsWith(lowerName) ||
                    candidate.replace(" ", "").startsWith(lowerName.replace(" ", ""))
            }
            .filter { file ->
                // If the caller explicitly requested NxEmu's preprocessed format (.dnsp/.dxci),
                // never silently repair it to raw .nsp/.xci.  This prevents paths with spaces or
                // truncated SAF/ADB arguments from loading the wrong sibling file.
                !requestedHasPreferredExt || file.extension.equals(requestedExt, ignoreCase = true)
            }
            .sortedWith(
                compareByDescending<File> { prefixRepairScore(it, lowerName, requestedExt) }
                    .thenBy { it.name.length }
                    .thenBy { it.name.lowercase(Locale.US) }
            )
            .toList()
        return matches.firstOrNull()
    }

    private fun prefixRepairScore(file: File, lowerName: String, requestedExt: String): Int {
        val name = file.name.lowercase(Locale.US)
        val ext = file.extension.lowercase(Locale.US)
        var score = 0
        if (name == lowerName) score += 1000
        if (requestedExt.isNotBlank() && ext == requestedExt) score += 500
        score += when (ext) {
            "dnsp" -> 300
            "dxci" -> 280
            "nro" -> 120
            "nca" -> 80
            "nsp" -> 20
            "xci" -> 10
            else -> 0
        }
        if ("[v0]" in name || "v0]" in name) score += 30
        return score
    }

    data class Result(
        val path: String,
        val original: String,
        val repaired: Boolean,
        val reason: String,
    )
}
