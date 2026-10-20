// SPDX-License-Identifier: MPL-2.0
package android.content
interface SharedPreferences {
    fun getBoolean(key: String, defValue: Boolean): Boolean
    fun edit(): Editor
    interface Editor { fun putBoolean(key: String, value: Boolean): Editor; fun apply() }
}
