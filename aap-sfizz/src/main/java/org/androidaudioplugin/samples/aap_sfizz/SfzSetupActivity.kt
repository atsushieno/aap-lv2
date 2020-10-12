package org.androidaudioplugin.samples.aap_sfizz
import android.os.Bundle
import android.provider.MediaStore
import android.util.Log
import android.widget.Button
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.io.FileOutputStream

class SfzSetupActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContentView(R.layout.sfz_setup)

        findViewById<Button>(R.id.copy_into_local_storage_button).setOnClickListener {
            Log.d("SfzSetupActivity", this.filesDir.absolutePath)
            var dst = File(filesDir.absolutePath + "/lv2")
            if (!dst.exists())
                dst.mkdirs()
            xcopyFromAssetsToLocalStorage (dst, "lv2")
        }
    }

    fun xcopyFromAssetsToLocalStorage(dst: File, src: String) {
        var list = assets.list(src)
        if (list == null)
            return
        if (list.any()) {
            if (!dst.exists())
                dst.mkdirs()
            // src is directory
            for (sub in list)
                xcopyFromAssetsToLocalStorage(File(dst, sub), src + "/" + sub)
        } else {
            // src is file
            FileOutputStream(dst).use { w ->
                assets.open(src).use { r ->
                    r.copyTo(w)
                }
            }
        }
    }
}