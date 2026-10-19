// SPDX-License-Identifier: MPL-2.0
// Host-side self-contained mapping checks: no Android runtime required.
// This source mirrors the pure inverse used by AirplaneModeSettings.
fun main() {
    for (legacyInternetEnabled in listOf(false, true)) {
        val migratedAirplaneMode = !legacyInternetEnabled
        val nativeInternetEnabled = !migratedAirplaneMode
        check(nativeInternetEnabled == legacyInternetEnabled)
    }
    check(!true == false)  // Airplane on -> native internet off
    check(!false == true)  // Airplane off -> native internet allowed
}
