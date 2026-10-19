// SPDX-License-Identifier: MPL-2.0
import android.content.Context
import androidx.preference.PreferenceManager
import org.stratoemu.strato.settings.AirplaneModeSettings
import org.stratoemu.strato.settings.AirplaneModePolicy

fun main() {
    verifyAirplaneModeMapping()
    val context = Context()
    val global = PreferenceManager.getDefaultSharedPreferences(context)
    val game = context.getSharedPreferences("game-1", Context.MODE_PRIVATE)

    // Legacy offline and online values must remain the same at the native ABI.
    global.edit().putBoolean("is_internet_enabled", false).apply()
    game.edit().putBoolean("is_internet_enabled", true).apply()

    AirplaneModeSettings.migrate(context)
    AirplaneModeSettings.migrate(context, "game-1")
    check(global.all[AirplaneModeSettings.KEY] == true)
    check(game.all[AirplaneModeSettings.KEY] == false)
    check(!AirplaneModePolicy.toNativeInternetEnabled(global.all[AirplaneModeSettings.KEY] as Boolean))
    check(AirplaneModePolicy.toNativeInternetEnabled(game.all[AirplaneModeSettings.KEY] as Boolean))

    // User changes persist as direct booleans and are not overwritten on launch.
    global.edit().putBoolean(AirplaneModeSettings.KEY, false).apply()
    game.edit().putBoolean(AirplaneModeSettings.KEY, true).apply()
    AirplaneModeSettings.migrate(context)
    AirplaneModeSettings.migrate(context, "game-1")
    check(global.all[AirplaneModeSettings.KEY] == false)
    check(game.all[AirplaneModeSettings.KEY] == true)

    // First install without the previous flag defaults to airplane mode on.
    AirplaneModeSettings.migrate(context, "new-game")
    val fresh = context.getSharedPreferences("new-game", Context.MODE_PRIVATE)
    check(fresh.all[AirplaneModeSettings.KEY] == true)
    println("Airplane Mode global/per-game preference migration: PASS")
}
