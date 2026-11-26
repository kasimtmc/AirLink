package com.plyg.airlink

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings
import android.util.Log

/**
 * Starts AirLink's service when the phone has started (after the first unlock: its data is in
 * credential-encrypted storage) and after AirLink was updated, so paired devices reach the phone
 * without opening the app. `connectedDevice` services may start from BOOT_COMPLETED (Android 15
 * only forbids data sync, media, camera, microphone and phone call ones there).
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED, ACTION_QUICKBOOT -> Unit
            else -> return
        }
        if (!enabled(context)) return
        try {
            AirLinkService.start(context)
        } catch (e: Exception) {
            Log.w("AirLink", "boot start: $e")
        }
    }

    companion object {
        private const val PREFS = "startup"
        private const val KEY = "start_at_boot"
        // Some vendors (HTC, older Xiaomi) send their own fast-boot action instead.
        private const val ACTION_QUICKBOOT = "android.intent.action.QUICKBOOT_POWERON"

        fun enabled(context: Context): Boolean =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY, true)

        fun setEnabled(context: Context, on: Boolean) =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY, on).apply()

        /** Doze and the battery saver may stop the connection while AirLink is optimized. */
        fun batteryRestricted(context: Context): Boolean {
            val power = context.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return false
            return !power.isIgnoringBatteryOptimizations(context.packageName)
        }

        /** The system's own "allow running in the background" question, else the app list. */
        @SuppressLint("BatteryLife")
        fun requestBatteryExemption(context: Context) {
            val ask = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:${context.packageName}"))
            try {
                context.startActivity(ask)
            } catch (_: Exception) {
                try {
                    context.startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
                } catch (e: Exception) {
                    Log.w("AirLink", "battery settings: $e")
                }
            }
        }
    }
}
