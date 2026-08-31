package org.nxemu.app

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.File

object NativeLibrary {
    private lateinit var appContext: Context

    init {
        System.loadLibrary("nxemu-android")
    }

    fun setApplicationContext(context: Context) {
        appContext = context.applicationContext
    }

    fun loadCoreModules(): List<String> {
        val modules = listOf(
            "nxemu-cpu",
            "nxemu-loader",
            "nxemu-video",
            "nxemu-os"
        )

        return modules.map { module ->
            runCatching {
                System.loadLibrary(module)
                "$module=loaded"
            }.getOrElse { error ->
                "$module=failed:${error::class.java.simpleName}:${error.message}"
            }
        }
    }

    @JvmStatic
    external fun initialize(nativeLibraryClass: Class<*>): Boolean

    @JvmStatic
    external fun version(): String

    @JvmStatic
    external fun initializeRuntime(filesDir: String, nativeLibraryDir: String): String

    @JvmStatic
    external fun initializeGpuDriver(
        hookLibDir: String,
        customDriverDir: String,
        customDriverName: String,
        fileRedirectDir: String
    ): String

    @JvmStatic
    external fun supportsCustomDriverLoading(): Boolean

    @JvmStatic
    external fun probeGame(path: String): String

    @JvmStatic
    external fun setSurface(surface: android.view.Surface?): String

    @JvmStatic
    external fun bootGame(path: String): String

    @JvmStatic
    external fun shutdownRuntime(): String

    @JvmStatic
    external fun runtimeStatus(): String

    @JvmStatic
    external fun nextLoadStatus(): String

    @JvmStatic
    external fun launchPendingNextLoad(): String

    @JvmStatic
    external fun setPlayerButton(playerIndex: Int, buttonOrdinal: Int, pressed: Boolean): String

    @JvmStatic
    external fun setPlayerAnalog(playerIndex: Int, stickIndex: Int, x: Float, y: Float): String

    @JvmStatic
    external fun requestGuestCpuSample(): String

    @JvmStatic
    external fun setPerformanceProfile(
        frameSkip: Int,
        resolutionSetup: Int,
        aspectRatio: Int,
        graphicsCompat: Boolean,
        preferNce: Boolean
    ): String

    @JvmStatic
    external fun getPerformanceStats(): String

    @JvmStatic
    external fun getLoadProgress(): String

    @JvmStatic
    fun getParentDirectory(path: String): String {
        return if (path.startsWith("content://")) {
            path.substringBeforeLast('/', missingDelimiterValue = "")
        } else {
            File(path).parent.orEmpty()
        }
    }

    @JvmStatic
    fun getFilename(path: String): String {
        return if (path.startsWith("content://")) {
            val uri = Uri.parse(path)
            runCatching {
                appContext.contentResolver.query(
                    uri,
                    arrayOf(OpenableColumns.DISPLAY_NAME),
                    null,
                    null,
                    null
                ).use { cursor ->
                    if (cursor != null && cursor.moveToFirst()) {
                        val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                        if (index >= 0) {
                            cursor.getString(index).orEmpty()
                        } else {
                            uri.lastPathSegment.orEmpty()
                        }
                    } else {
                        uri.lastPathSegment.orEmpty()
                    }
                }
            }.getOrDefault(uri.lastPathSegment.orEmpty())
        } else {
            File(path).name
        }
    }

    @JvmStatic
    fun getSize(path: String): Long {
        return runCatching {
            if (path.startsWith("content://")) {
                val uri = Uri.parse(path)
                appContext.contentResolver.query(
                    uri,
                    arrayOf(DocumentsContract.Document.COLUMN_SIZE),
                    null,
                    null,
                    null
                ).use { cursor ->
                    if (cursor != null && cursor.moveToFirst()) {
                        val index = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_SIZE)
                        if (index >= 0 && !cursor.isNull(index)) {
                            cursor.getLong(index)
                        } else {
                            appContext.contentResolver.openAssetFileDescriptor(uri, "r")?.use {
                                it.length
                            } ?: 0L
                        }
                    } else {
                        0L
                    }
                }
            } else {
                File(path).length()
            }
        }.getOrDefault(0L)
    }

    @JvmStatic
    fun isDirectory(path: String): Boolean {
        return runCatching {
            if (path.startsWith("content://")) {
                val uri = Uri.parse(path)
                appContext.contentResolver.query(
                    uri,
                    arrayOf(DocumentsContract.Document.COLUMN_MIME_TYPE),
                    null,
                    null,
                    null
                ).use { cursor ->
                    if (cursor != null && cursor.moveToFirst()) {
                        val index = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
                        index >= 0 && cursor.getString(index) == DocumentsContract.Document.MIME_TYPE_DIR
                    } else {
                        false
                    }
                }
            } else {
                File(path).isDirectory
            }
        }.getOrDefault(false)
    }

    @JvmStatic
    fun exists(path: String): Boolean {
        return runCatching {
            if (path.startsWith("content://")) {
                val uri = Uri.parse(path)
                appContext.contentResolver.query(
                    uri,
                    arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID),
                    null,
                    null,
                    null
                ).use { cursor ->
                    cursor != null && cursor.count > 0
                }
            } else {
                File(path).exists()
            }
        }.getOrDefault(false)
    }

    @JvmStatic
    fun openContentUri(path: String, mode: String): Int {
        return runCatching {
            if (path.startsWith("content://")) {
                appContext.contentResolver.openFileDescriptor(Uri.parse(path), mode)?.detachFd()
            } else {
                ParcelFileDescriptor.open(File(path), ParcelFileDescriptor.MODE_READ_ONLY).detachFd()
            } ?: -1
        }.getOrDefault(-1)
    }
}
