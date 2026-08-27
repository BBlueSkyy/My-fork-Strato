package org.stratoemu.strato

import android.app.Application
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.loader.RomType
import org.stratoemu.strato.utils.fromFile
import org.stratoemu.strato.utils.toFile
import java.io.File
import javax.inject.Inject

sealed class MainState {
    object Loading : MainState()
    class Loaded(val items: ArrayList<AppEntry>) : MainState()
    class Error(val ex: Exception) : MainState()
}

@HiltViewModel
class MainViewModel @Inject constructor(
    @ApplicationContext context: Context,
    private val romProvider: RomProvider
) : AndroidViewModel(context as Application) {

    companion object {
        private val TAG = MainViewModel::class.java.simpleName
    }

    private var state: MainState?
        get() = _stateData.value
        set(value) = _stateData.postValue(value!!)

    private val _stateData = MutableLiveData<MainState>()
    val stateData: LiveData<MainState> = _stateData

    fun loadRoms(
        context: Context,
        loadFromFile: Boolean,
        searchLocations: List<Uri>,
        systemLanguage: Int
    ) {
        if (state == MainState.Loading)
            return

        state = MainState.Loading

        val romsFile = File(
            getApplication<StratoApplication>().filesDir.canonicalPath + "/roms.bin"
        )

        viewModelScope.launch(Dispatchers.IO) {
            if (loadFromFile && romsFile.exists()) {
                try {
                    state = MainState.Loaded(fromFile(romsFile))
                    checkRomHash(searchLocations, systemLanguage)
                    return@launch
                } catch (e: Exception) {
                    Log.w(TAG, "Ran into exception while loading: ${e.message}")
                }
            }

            state = if (searchLocations.isEmpty()) {
                MainState.Loaded(ArrayList())
            } else {
                try {
                    searchLocations.forEach { searchLocation ->
                        try {
                            KeyReader.importFromLocation(context, searchLocation)
                        } catch (e: Exception) {
                            Log.w(
                                TAG,
                                "Unable to inspect keys in $searchLocation: ${e.message}"
                            )
                        }
                    }

                    val romElements =
                        romProvider.loadRoms(searchLocations, systemLanguage)

                    val gameTitleIds = romElements
                        .filter {
                            it.romType == RomType.Base ||
                                it.romType == RomType.Unknown
                        }
                        .mapNotNull { it.titleId }
                        .toSet()

                    gameTitleIds.forEach { titleId ->
                        try {
                            val (removedUpdates, removedDlcs) =
                                ContentManager.cleanupNonExistentContent(
                                    context,
                                    titleId
                                )

                            if (removedUpdates > 0 || removedDlcs > 0) {
                                Log.d(
                                    TAG,
                                    "Cleaned up content for game $titleId: " +
                                        "$removedUpdates updates and " +
                                        "$removedDlcs DLCs removed"
                                )
                            }
                        } catch (e: Exception) {
                            Log.e(
                                TAG,
                                "Error cleaning up content for game $titleId",
                                e
                            )
                        }
                    }

                    romElements.toFile(romsFile)
                    MainState.Loaded(romElements)
                } catch (e: Exception) {
                    Log.w(TAG, "Ran into exception while saving: ${e.message}")
                    MainState.Error(e)
                }
            }
        }
    }

    private var isAutoRefreshingRoms = false

    fun checkRomHash(
        searchLocations: List<Uri>,
        systemLanguage: Int
    ) {
        if (isAutoRefreshingRoms || state !is MainState.Loaded)
            return

        isAutoRefreshingRoms = true

        viewModelScope.launch(Dispatchers.IO) {
            try {
                val currentHash = when (val currentState = state) {
                    is MainState.Loaded -> currentState.items.hashCode()
                    else -> 0
                }

                val romElements =
                    romProvider.loadRoms(searchLocations, systemLanguage)
                val newHash = romElements.hashCode()

                if (newHash != currentHash) {
                    state = MainState.Loaded(romElements)
                }
            } catch (e: Exception) {
                Log.w(TAG, "Unable to refresh ROM list: ${e.message}")
            } finally {
                isAutoRefreshingRoms = false
            }
        }
    }
}
