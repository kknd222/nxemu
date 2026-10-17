package org.nxemu.features.saves

import android.app.AlertDialog
import android.app.ProgressDialog
import android.net.Uri
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.AndroidDevice
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary
import org.nxemu.ui.emulation.EmulationActivity
import org.nxemu.ui.main.MainActivity
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

/** SAF pickers stay on the UI thread; archive IO never runs on WebView or render threads. */
@Suppress("DEPRECATION")
class SaveDataManager(private val activity: MainActivity) {
    private data class Save(val titleId: String, val kind: String, val path: File, val title: String)
    private var pendingExport: Save? = null

    private val importPicker = activity.registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) stageImport(uri)
    }
    private val exportPicker = activity.registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        val save = pendingExport
        pendingExport = null
        if (uri != null && save != null) {
            work("正在导出存档…") {
                activity.contentResolver.openOutputStream(uri, "wt")?.use { output ->
                    SaveArchive.exportZip(save.path, save.titleId, save.kind, output)
                } ?: error("无法写入选择的文件")
                "存档导出完成"
            }
        }
    }

    fun importSave() {
        if (available()) importPicker.launch(arrayOf("application/zip", "application/octet-stream", "application/x-zip-compressed"))
    }

    fun restoreBackup() {
        if (!available()) return
        val files = File(AndroidDevice.PACKAGE_DIRECTORY, "save_backups").listFiles()
            ?.filter { it.isFile && it.extension == "zip" }?.sortedByDescending { it.lastModified() }.orEmpty()
        if (files.isEmpty()) { message("没有自动备份"); return }
        val date = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.getDefault())
        AlertDialog.Builder(activity).setTitle("选择要恢复的自动备份")
            .setItems(files.map { "${it.name.take(24)} · ${date.format(java.util.Date(it.lastModified()))}" }.toTypedArray()) { _, index ->
                stageImport(Uri.fromFile(files[index]))
            }.setNegativeButton(android.R.string.cancel, null).show()
    }

    fun exportSave() {
        if (!available() || !busy.compareAndSet(false, true)) return
        val progress = progress("正在查找游戏存档…")
        Thread({
            try {
                val saves = mutableListOf<Save>()
                val paths = JSONArray(GameLibraryScanner.scanRomUris(activity.applicationContext))
                val seen = mutableSetOf<String>()
                for (i in 0 until paths.length()) {
                    val meta = JSONObject(NativeLibrary.queryRomMetadata(paths.getString(i)))
                    val id = meta.optString("programId").uppercase()
                    if (!id.matches(Regex("[0-9A-F]{16}")) || !seen.add(id)) continue
                    val resolved = resolve(id)
                    for (kind in listOf("account", "device")) {
                        val directory = File(resolved.getString(kind))
                        if (directory.isDirectory && directory.list()?.isNotEmpty() == true) {
                            saves += Save(id, kind, directory, meta.optString("title", id))
                        }
                    }
                }
                activity.runOnUiThread {
                    progress.dismiss()
                    busy.set(false)
                    if (activity.isDestroyed) return@runOnUiThread
                    if (saves.isEmpty()) {
                        message("没有找到存档。请先添加游戏目录，并在游戏中保存一次。")
                    } else {
                        AlertDialog.Builder(activity).setTitle("选择要导出的游戏存档")
                            .setItems(saves.map { "${it.title}\n${it.titleId} · ${kindLabel(it.kind)}" }.toTypedArray()) { _, index ->
                                if (available()) {
                                    pendingExport = saves[index]
                                    exportPicker.launch("${saves[index].titleId}-${saves[index].kind}.zip")
                                }
                            }.setNegativeButton(android.R.string.cancel, null).show()
                    }
                }
            } catch (error: Exception) {
                activity.runOnUiThread {
                    progress.dismiss(); busy.set(false); message(error.message ?: "读取存档失败")
                }
            }
        }, "SaveList").start()
    }

    private fun stageImport(uri: Uri) {
        if (!available() || !busy.compareAndSet(false, true)) return
        val stage = File(File(AndroidDevice.PACKAGE_DIRECTORY, "save_import_tmp"), UUID.randomUUID().toString())
        val progress = progress("正在检查存档 ZIP…")
        Thread({
            try {
                val bundle = activity.contentResolver.openInputStream(uri)?.use { SaveArchive.unpack(it, stage) }
                    ?: error("无法读取选择的 ZIP")
                val paths = resolve(bundle.titleId)
                val destination = File(paths.getString(bundle.kind))
                activity.runOnUiThread {
                    progress.dismiss()
                    if (activity.isDestroyed) { clean(stage); return@runOnUiThread }
                    AlertDialog.Builder(activity).setTitle("导入游戏存档")
                        .setMessage("Title ID：${bundle.titleId}\n类型：${kindLabel(bundle.kind)}\n\n将导入当前用户的该游戏存档。已有存档会先备份，再完整替换。备份保存在应用数据目录 save_backups 中，可用同一导入入口恢复。")
                        .setPositiveButton("备份并导入") { _, _ -> commitImport(bundle, destination, stage) }
                        .setNegativeButton(android.R.string.cancel) { _, _ -> clean(stage) }
                        .setOnCancelListener { clean(stage) }.show()
                }
            } catch (error: Exception) {
                activity.runOnUiThread {
                    progress.dismiss(); message(error.message ?: "ZIP 检查失败"); clean(stage)
                }
            }
        }, "SaveImportCheck").start()
    }

    private fun commitImport(bundle: SaveArchive.Bundle, destination: File, stage: File) {
        // The busy flag also blocks launching a game between confirmation and replacement.
        if (EmulationActivity.activeGamePath != null) { message("请先关闭游戏"); clean(stage); return }
        val progress = progress("正在备份并导入存档…")
        Thread({
            val result = try {
                val backup = SaveArchive.replace(destination, bundle, File(AndroidDevice.PACKAGE_DIRECTORY, "save_backups"))
                if (backup == null) "存档导入完成" else "存档导入完成，原存档备份：${backup.name}"
            } catch (error: Exception) { error.message ?: "存档导入失败，未能完成替换" }
            try { SaveArchive.deleteTree(stage) } catch (_: Exception) { /* staging only */ } finally { busy.set(false) }
            activity.runOnUiThread { progress.dismiss(); message(result) }
        }, "SaveImport").start()
    }

    private fun resolve(titleId: String): JSONObject {
        val json = NativeLibrary.getSaveDataPaths(titleId)
        check(json.isNotEmpty()) { "无法解析当前用户存档路径，请先关闭游戏" }
        return JSONObject(json)
    }

    private fun work(label: String, action: () -> String) {
        if (!available() || !busy.compareAndSet(false, true)) return
        val progress = progress(label)
        Thread({
            val result = try { action() } catch (error: Exception) { error.message ?: "存档操作失败" }
            busy.set(false)
            activity.runOnUiThread { progress.dismiss(); message(result) }
        }, "SaveExport").start()
    }

    private fun clean(stage: File) {
        Thread({ try { SaveArchive.deleteTree(stage) } finally { busy.set(false) } }, "SaveCleanup").start()
    }
    private fun available(): Boolean {
        if (EmulationActivity.activeGamePath != null) { message("请先关闭游戏，再管理存档"); return false }
        if (busy.get()) { message("存档操作进行中，请稍候"); return false }
        return true
    }
    private fun progress(label: String): ProgressDialog = ProgressDialog(activity).apply {
        setMessage(label); setCancelable(false); show()
    }
    private fun message(value: String) { if (!activity.isDestroyed) Toast.makeText(activity, value, Toast.LENGTH_LONG).show() }
    private fun kindLabel(kind: String) = if (kind == "device") "设备存档" else "账号存档"

    companion object {
        private val busy = AtomicBoolean(false)
        fun isBusy(): Boolean = busy.get()
    }
}
