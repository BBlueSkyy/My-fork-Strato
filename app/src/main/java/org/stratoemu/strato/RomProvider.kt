package org.stratoemu.strato

import android.annotation.SuppressLint
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.loader.RomFile
import org.stratoemu.strato.loader.RomFormat
import org.stratoemu.strato.loader.RomFormat.*
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class RomProvider @Inject constructor(
    @ApplicationContext private val context: Context
) {
    companion object {
        private val TAG = RomProvider::class.java.simpleName

        private val ROM_FORMATS = mapOf(
            "nro" to NRO,
            "nso" to NSO,
            "nca" to NCA,
            "nsp" to NSP,
            "xci" to XCI
        )
    }

    @SuppressLint("DefaultLocale")
    private fun addEntries(
        fileFormats: Map<String, RomFormat>,
        directory: DocumentFile,
        entries: ArrayList<AppEntry>,
        seenUris: MutableSet<String>,
        systemLanguage: Int
    ) {
        val files = try {
            directory.listFiles()
        } catch (e: Exception) {
            Log.w(TAG, "Unable to list ROM directory ${directory.uri}: ${e.message}")
            return
        }

        files.forEach { file ->
            if (file.isDirectory) {
                addEntries(fileFormats, file, entries, seenUris, systemLanguage)
            } else {
                val extension = file.name
                    ?.substringAfterLast(".", "")
                    ?.lowercase()

                fileFormats[extension]?.let { romFormat ->
                    val uriString = file.uri.toString()

                    if (seenUris.add(uriString)) {
                        try {
                            entries.add(
                                RomFile(
                                    context,
                                    romFormat,
                                    file.uri,
                                    systemLanguage
                                ).appEntry
                            )
                        } catch (e: Exception) {
                            Log.w(TAG, "Unable to load ROM $uriString: ${e.message}")
                        }
                    }
                }
            }
        }
    }

    fun loadRoms(
        searchLocations: Collection<Uri>,
        systemLanguage: Int
    ): ArrayList<AppEntry> {
        val entries = arrayListOf<AppEntry>()
        val seenUris = mutableSetOf<String>()

        searchLocations
            .filter { it.toString().isNotBlank() }
            .distinctBy { it.toString() }
            .forEach { searchLocation ->
                val documentFile = try {
                    DocumentFile.fromTreeUri(context, searchLocation)
                } catch (e: Exception) {
                    Log.w(TAG, "Unable to open ROM directory $searchLocation: ${e.message}")
                    null
                }

                if (documentFile == null || !documentFile.isDirectory) {
                    Log.w(TAG, "Skipping unavailable ROM directory: $searchLocation")
                    return@forEach
                }

                addEntries(
                    ROM_FORMATS,
                    documentFile,
                    entries,
                    seenUris,
                    systemLanguage
                )
            }

        return entries
    }

    fun loadRoms(
        searchLocation: Uri,
        systemLanguage: Int
    ): ArrayList<AppEntry> = loadRoms(listOf(searchLocation), systemLanguage)
}
