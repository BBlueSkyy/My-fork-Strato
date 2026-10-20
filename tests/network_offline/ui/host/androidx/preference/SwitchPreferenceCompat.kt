// SPDX-License-Identifier: MPL-2.0
package androidx.preference
class SwitchPreferenceCompat {
    var isPersistent = true
    var isChecked = false
    private var listener: ((SwitchPreferenceCompat, Any) -> Boolean)? = null
    fun setOnPreferenceChangeListener(cb: (SwitchPreferenceCompat, Any) -> Boolean) {
        listener = cb
    }
    fun click(value: Boolean): Boolean {
        val accepted = listener?.invoke(this, value) ?: true
        if (accepted) isChecked = value
        return accepted
    }
}
