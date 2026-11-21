package com.dna.tools

import android.content.Context
import java.io.File

class DebugConfig(context: Context) {
    private val appContext = context.applicationContext
    private val config = appContext.getSharedPreferences("dna-settings", Context.MODE_PRIVATE)

    fun isEnabled(): Boolean {
        return config.getBoolean("debug_mode", false)
    }

    fun setEnabled(enabled: Boolean) {
        config.edit().putBoolean("debug_mode", enabled).apply()
        sync()
    }

    fun sync() {
        try {
            File(appContext.filesDir, "DNA.debug").writeText(if (isEnabled()) "1" else "0")
        } catch (_: Exception) {
        }
    }
}
