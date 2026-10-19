// SPDX-License-Identifier: MPL-2.0
package androidx.preference

import android.content.Context
import android.content.SharedPreferences

object PreferenceManager {
    fun getDefaultSharedPreferences(context: Context): SharedPreferences =
        context.getSharedPreferences("global", Context.MODE_PRIVATE)
}
