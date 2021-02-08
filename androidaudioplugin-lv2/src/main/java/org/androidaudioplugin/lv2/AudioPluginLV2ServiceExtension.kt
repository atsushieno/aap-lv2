package org.androidaudioplugin.lv2

import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.res.AssetManager
import org.androidaudioplugin.AudioPluginLocalHost
import org.androidaudioplugin.AudioPluginService
import java.io.File

// This class is (can be) used as an AudioPluginHost extension.
class AudioPluginLV2ServiceExtension : AudioPluginService.Extension
{
    companion object {
        init {
            System.loadLibrary("androidaudioplugin")
            System.loadLibrary("androidaudioplugin-lv2")
        }
    }

    val LV2_RESOURCE_FROM_FILE = "org.androidaudioplugin.lv2.AudioPluginLV2ServiceExtension#ResourceFromFile"

    override fun initialize(context: Context)
    {
        var lv2Paths = AudioPluginLocalHost.getLocalAudioPluginService(context).plugins
            .filter { p -> p.backend == "LV2" }
            .map { p -> if(p.assets != null) File(p.assets!!).parent.toString() else "" }
            .distinct().toTypedArray()
        val svcInfo = context.packageManager.getServiceInfo(
            ComponentName(context, AudioPluginService::class.java), 0)
        val isFile = svcInfo.metaData?.getBoolean(LV2_RESOURCE_FROM_FILE, false)
        val dataAbsDir = context.applicationContext.filesDir.absolutePath
        if (isFile == true) {
            var lv2pathStr = lv2Paths.joinToString(":") {
                    s -> if (s.startsWith('/')) "$dataAbsDir$s" else "$dataAbsDir/$s" }
            initialize(lv2pathStr, null)
        }
        else
            initialize(lv2Paths.joinToString(":"), context.assets)
    }

    private external fun initialize(lv2Path: String, assets: AssetManager?)

    external override fun cleanup()
}