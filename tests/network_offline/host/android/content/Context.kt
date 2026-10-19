// SPDX-License-Identifier: MPL-2.0
package android.content

class SharedPreferences {
    private val values = mutableMapOf<String, Any>()
    val all: Map<String, Any> get() = values.toMap()

    fun edit() = Editor(values)

    class Editor(private val values: MutableMap<String, Any>) {
        fun putBoolean(key: String, value: Boolean) = apply { values[key] = value }
        fun apply() {}
    }
}

class Context {
    companion object { const val MODE_PRIVATE = 0 }
    private val stores = mutableMapOf<String, SharedPreferences>()
    fun getSharedPreferences(name: String, mode: Int): SharedPreferences =
        stores.getOrPut(name) { SharedPreferences() }
}
