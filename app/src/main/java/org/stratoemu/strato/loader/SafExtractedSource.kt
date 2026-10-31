package org.stratoemu.strato.loader

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract

/**
 * Read-only view of a decrypted Switch directory reached through an Android
 * persisted SAF tree grant. Never assumes the document URI is a filesystem path.
 */
class SafExtractedSource private constructor(private val context: Context, val rootUri: Uri, scanNow: Boolean) {
    private data class Entry(val path: String, val uri: Uri, val size: Long)
    private data class Child(val name: String, val uri: Uri, val directory: Boolean, val size: Long)
    private val entries = ArrayList<Entry>()
    private val documentProjection = arrayOf(
        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE,
        DocumentsContract.Document.COLUMN_SIZE
    )

    private fun children(parent: Uri): List<Child> {
        val parentId = DocumentsContract.getDocumentId(parent)
        val childUri = DocumentsContract.buildChildDocumentsUriUsingTree(parent, parentId)
        val result = ArrayList<Child>()
        context.contentResolver.query(childUri, documentProjection, null, null, null)?.use { cursor ->
            while (cursor.moveToNext()) {
                val id = cursor.getString(0)
                val name = cursor.getString(1) ?: continue
                // Avoid path injection and duplicate names in virtual filesystems.
                if (name.isEmpty() || name == "." || name == ".." ||
                    '/' in name || '\\' in name || '\u0000' in name) continue
                val mime = cursor.getString(2)
                val size = if (cursor.isNull(3)) -1L else cursor.getLong(3)
                result.add(Child(name,
                    DocumentsContract.buildDocumentUriUsingTree(parent, id),
                    mime == DocumentsContract.Document.MIME_TYPE_DIR, size))
            }
        } ?: throw IllegalStateException("Cannot enumerate extracted game folder: $parent")
        return result
    }

    companion object {
        /** Cheap detection during ordinary recursive ROM library scans. */
        fun isExtractedDirectory(context: Context, directory: Uri): Boolean = try {
            val view = SafExtractedSource(context, directory, false)
            val exeFs = view.children(directory).firstOrNull { it.directory && it.name.equals("exefs", true) }
            exeFs != null && view.children(exeFs.uri)
                .filter { !it.directory }.map { it.name }.containsAll(listOf("main", "rtld", "main.npdm"))
        } catch (_: Exception) {
            false
        }

        fun npdmUri(context: Context, root: Uri): Uri {
            val source = SafExtractedSource(context, root, false)
            val exefs = source.children(root).first { it.directory && it.name.equals("exefs", true) }
            return source.children(exefs.uri).first { !it.directory && it.name == "main.npdm" }.uri
        }
    }

    constructor(context: Context, root: Uri) : this(context, root, true)

    init { if (scanNow) scan() }

    private fun scan() {
        entries.clear()
        val queue = ArrayDeque<Pair<Uri, String>>()
        queue.add(rootUri to "")
        val seen = HashSet<String>()
        while (queue.isNotEmpty()) {
            val (directory, prefix) = queue.removeFirst()
            if (prefix.count { it == '/' } > 64)
                throw IllegalArgumentException("Extracted game directory nesting is too deep")
            for (child in children(directory)) {
                val path = prefix + child.name
                if (!seen.add(path)) throw IllegalArgumentException("Duplicate file path in extracted game: $path")
                if (child.directory) {
                    queue.add(child.uri to "$path/")
                } else {
                    if (entries.size >= 200000)
                        throw IllegalArgumentException("Extracted game contains too many files")
                    if (child.size < 0)
                        throw IllegalArgumentException("Document provider has no file size for $path")
                    // Ignore unrelated files; only ExeFS, RomFS and optional NACP are exposed.
                    if (path.startsWith("exefs/", true) || path.startsWith("romfs/", true) ||
                        path.equals("control.nacp", true))
                        entries.add(Entry(path, child.uri, child.size))
                }
            }
        }
        if (!entries.any { it.path == "exefs/main" } ||
            !entries.any { it.path == "exefs/rtld" } ||
            !entries.any { it.path == "exefs/main.npdm" })
            throw IllegalArgumentException("Extracted game lacks main, rtld or main.npdm")
    }

    val paths: Array<String> get() = entries.map { it.path }.toTypedArray()
    val sizes: LongArray get() = entries.map { it.size }.toLongArray()
    val title: String get() = rootUri.lastPathSegment?.substringAfterLast('/') ?: "Extracted game"

    fun openMain(): ParcelFileDescriptor {
        val entry = entries.first { it.path == "exefs/main" }
        return context.contentResolver.openFileDescriptor(entry.uri, "r")
            ?: throw IllegalStateException("Cannot open extracted ExeFS main")
    }

    /** JNI owns and closes the detached descriptor. Only called on-demand. */
    fun openFile(index: Int): Int {
        val entry = entries[index]
        return context.contentResolver.openFileDescriptor(entry.uri, "r")?.detachFd()
            ?: throw IllegalStateException("Cannot open extracted game file: ${entry.path}")
    }
}
