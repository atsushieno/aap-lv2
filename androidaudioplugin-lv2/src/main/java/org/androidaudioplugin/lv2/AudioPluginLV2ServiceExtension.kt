package org.androidaudioplugin.lv2

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.content.res.AssetManager
import org.androidaudioplugin.AudioPluginService
import org.androidaudioplugin.hosting.AudioPluginHostHelper
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
        val svcInfo = context.packageManager.getServiceInfo(
            ComponentName(context, AudioPluginService::class.java), PackageManager.GET_META_DATA)
        val isFile = svcInfo.metaData?.getBoolean(LV2_RESOURCE_FROM_FILE, false)
        val dataAbsDir = context.applicationContext.filesDir.canonicalPath
        if (isFile == true) {
            val lv2pathStr = "$dataAbsDir/lv2"
            initialize(lv2pathStr, null)
        } else {
            val lv2 = context.assets.list("lv2")
            val paths = lv2?.map { "/lv2/$it/" }?.toTypedArray() ?: arrayOf()
            val lv2Paths = paths.joinToString(":")
            initialize(lv2Paths, context.assets)
        }
    }

    private external fun initialize(lv2Path: String, assets: AssetManager?)

    external override fun cleanup()
}