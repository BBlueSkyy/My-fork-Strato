// SPDX-License-Identifier: MPL-2.0
import org.stratoemu.strato.settings.AirplaneModePolicy

fun main() {
    // Offline must remain offline during upgrades; online must remain online.
    for (legacyInternetEnabled in listOf(false, true)) {
        val migrated = AirplaneModePolicy.fromLegacyInternetEnabled(legacyInternetEnabled)
        val nativeValue = AirplaneModePolicy.toNativeInternetEnabled(migrated)
        check(nativeValue == legacyInternetEnabled)
    }

    check(AirplaneModePolicy.fromLegacyInternetEnabled(false))
    check(!AirplaneModePolicy.fromLegacyInternetEnabled(true))
    check(!AirplaneModePolicy.toNativeInternetEnabled(true))
    check(AirplaneModePolicy.toNativeInternetEnabled(false))
    println("Airplane Mode migration and native mapping: PASS")
}
