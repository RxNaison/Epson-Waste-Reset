package io.github.rxnaison.ewr

import android.os.Bundle
import android.system.Os
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import io.github.rxnaison.ewr.ui.EwrApp
import io.github.rxnaison.ewr.ui.EwrTheme

class MainActivity : ComponentActivity() {

    private val viewModel: EwrViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        // Before any session: ewr_trace.log goes to the app's own files, and
        // the one-run-at-a-time lock to its cache, the only places it may write.
        EwrNative.setDataDir(filesDir.path)
        Os.setenv("TMPDIR", cacheDir.path, true)

        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            EwrTheme {
                EwrApp(viewModel)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Plugging the printer in starts this activity (USB_DEVICE_ATTACHED).
        viewModel.refreshUsb()
    }
}
