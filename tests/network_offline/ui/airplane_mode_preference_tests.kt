// SPDX-License-Identifier: MPL-2.0
import android.content.SharedPreferences
import androidx.preference.SwitchPreferenceCompat
import org.stratoemu.strato.preference.AirplaneModePreferenceBinder

class FakeNetworkPrefs : SharedPreferences {
    val values = mutableMapOf<String, Boolean>()
    override fun getBoolean(key: String, defValue: Boolean) = values[key] ?: defValue
    override fun edit() = object : SharedPreferences.Editor {
        override fun putBoolean(key: String, value: Boolean): SharedPreferences.Editor {
            values[key] = value
            return this
        }
        override fun apply() {}
    }
}

fun main() {
    for (internetEnabled in listOf(false, true)) {
        val prefs = FakeNetworkPrefs()
        prefs.values["is_internet_enabled"] = internetEnabled
        val toggle = SwitchPreferenceCompat()
        AirplaneModePreferenceBinder.bind(toggle, prefs)
        check(toggle.isChecked == !internetEnabled)
        check(!toggle.isPersistent)
        check(prefs.values["is_internet_enabled"] == internetEnabled)

        for (airplaneMode in listOf(true, false, true)) {
            check(toggle.click(airplaneMode))
            check(prefs.values["is_internet_enabled"] == !airplaneMode)
        }
    }
    val global = FakeNetworkPrefs()
    val game = FakeNetworkPrefs().apply { values["is_internet_enabled"] = true }
    val globalSwitch = SwitchPreferenceCompat()
    val gameSwitch = SwitchPreferenceCompat()
    AirplaneModePreferenceBinder.bind(globalSwitch, global)
    AirplaneModePreferenceBinder.bind(gameSwitch, game)
    check(globalSwitch.isChecked && !gameSwitch.isChecked)
    check(global.values.isEmpty())
    globalSwitch.click(false)
    check(global.values["is_internet_enabled"] == true)
    check(game.values["is_internet_enabled"] == true)
    gameSwitch.click(true)
    check(game.values["is_internet_enabled"] == false)
    check(global.values["is_internet_enabled"] == true)
    println("Airplane Mode UI -> legacy native network setting: PASS")
}
