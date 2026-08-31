package org.nxemu.app

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract

object SafScanner {
    fun scanDocuments(
        contentResolver: ContentResolver,
        rootUri: Uri,
        maxDepth: Int,
        acceptFile: (name: String, mime: String) -> Boolean
    ): List<Uri> {
        val results = mutableListOf<Uri>()

        fun isRootTreeUri(uri: Uri): Boolean {
            val segments = uri.pathSegments
            return segments.size == 2 && segments[0] == "tree"
        }

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
                DocumentsContract.Document.COLUMN_MIME_TYPE
            )
            contentResolver.query(childrenUri, columns, null, null, null).use { cursor ->
                if (cursor == null) {
                    return
                }
                val idIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val mimeIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
                while (cursor.moveToNext()) {
                    val childId = cursor.getString(idIndex)
                    val name = cursor.getString(nameIndex).orEmpty()
                    val mime = cursor.getString(mimeIndex).orEmpty()
                    val childUri = DocumentsContract.buildDocumentUriUsingTree(rootUri, childId)
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                        scan(childUri, depth - 1)
                    } else if (acceptFile(name, mime)) {
                        results += childUri
                    }
                }
            }
        }

        scan(rootUri, maxDepth)
        return results
    }
}
