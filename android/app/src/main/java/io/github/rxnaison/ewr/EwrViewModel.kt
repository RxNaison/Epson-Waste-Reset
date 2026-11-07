package io.github.rxnaison.ewr

import android.app.Application
import android.net.Uri
import android.net.wifi.WifiManager
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/**
 * The app's one screen of state. Every printer operation runs off the main
 * thread on a session of its own, exactly as the C ABI is meant to be used:
 * open, one call, close. The gates and the confirmation become dialogs; the
 * native call waits on the answer, and no answer is a no.
 */
class EwrViewModel(application: Application) : AndroidViewModel(application) {

    private val app = application
    val usb = UsbPrinters(application)
    val storage = Storage(application)

    /** Empty for USB, otherwise the network printer's address. */
    var host by mutableStateOf("")
    var usbNames by mutableStateOf<List<String>>(emptyList())
    var reported by mutableStateOf<String?>(null)
    var model by mutableStateOf<String?>(null)
    var models by mutableStateOf<List<String>>(emptyList())
    var networkPrinters by mutableStateOf<List<Pair<String, String>>>(emptyList())
    var status by mutableStateOf<JSONObject?>(null)
    var info by mutableStateOf<JSONObject?>(null)
    var backups by mutableStateOf<List<File>>(emptyList())
    var busy by mutableStateOf<String?>(null)
    var progress by mutableStateOf<Float?>(null)
    var outcome by mutableStateOf<String?>(null)
    var question by mutableStateOf<Question?>(null)
    val log = mutableStateListOf<String>()

    sealed class Question(val title: String, val text: String, val answer: CompletableDeferred<Boolean>) {
        class Gate(title: String, text: String, answer: CompletableDeferred<Boolean>) : Question(title, text, answer)
        class Confirm(title: String, text: String, val typedWord: String?, answer: CompletableDeferred<Boolean>) :
            Question(title, text, answer)
    }

    init {
        storage.seedDatabase()
        backups = storage.backups()
        refreshUsb()
    }

    // ---- What the native side hears back -------------------------------------

    private val callbacks = object : EwrCallbacks {
        override fun onEvent(code: String, message: String, level: Int, stage: Int, index: Int, total: Int,
                             fieldsJson: String) {
            // Level 0 is the trace: ewr_trace.log's business, not the screen's.
            if (level < 1)
                return
            viewModelScope.launch(Dispatchers.Main) {
                val line = message.trim()
                if (line.isNotEmpty()) {
                    log.add(line)
                    if (log.size > 400)
                        log.removeRange(0, log.size - 400)
                }
                progress = if (total > 0) index.toFloat() / total else progress
            }
        }

        override fun decideBlocker(json: String): Boolean {
            val gate = JSONObject(json)
            val error = gate.optString("error")
            val title = if (gate.isNull("error_code")) error else "The printer reports $error"
            return ask { Question.Gate(title, gate.optString("explanation").trimIndent(), it) }
        }

        override fun confirm(json: String): Boolean {
            val ask = JSONObject(json)
            val name = model ?: "the printer"
            return when (ask.optString("target")) {
                "ink" -> ask {
                    Question.Confirm(
                        "Reset the cartridge ink levels?",
                        "EWR cannot refill ink: an empty cartridge will report full and can run dry " +
                            "mid-print, which damages the print head. It holds only where the levels live " +
                            "in the printer's own memory, not on a cartridge chip.",
                        "reset", it,
                    )
                }
                "restore" -> ask {
                    Question.Confirm("Write the backup back to $name?",
                        "Only the counter bytes that differ are written, and EWR keeps a backup of what " +
                            "they hold now.", null, it)
                }
                "head-clean" -> ask {
                    Question.Confirm("Clean the print head now?",
                        "A cleaning uses ink, and the ink it uses goes into the waste ink pads.", null, it)
                }
                "power-clean" -> ask {
                    Question.Confirm("Run a power cleaning?",
                        "The ink charge uses a large amount of ink and fills the waste ink pads quickly. " +
                            "Run it only when normal cleanings did not clear the nozzles.", "charge", it)
                }
                "nozzle-check" -> ask {
                    Question.Confirm("Print the nozzle check?", "Load plain paper: the printer prints one page.",
                        null, it)
                }
                else -> ask {
                    Question.Confirm("Reset the waste ink pad counters of $name?",
                        "EWR keeps a backup of every byte it overwrites, so this can be undone from Backups.",
                        null, it)
                }
            }
        }

        override fun saveBackup(json: String): Boolean {
            val file = storage.saveBackup(json) ?: return false
            viewModelScope.launch(Dispatchers.Main) {
                log.add("[*] Backup kept: ${file.name}")
                backups = storage.backups()
            }
            return true
        }
    }

    /** Puts a question on screen and blocks the calling native thread on it. */
    private fun ask(make: (CompletableDeferred<Boolean>) -> Question): Boolean = runBlocking {
        val answer = CompletableDeferred<Boolean>()
        withContext(Dispatchers.Main) { question = make(answer) }
        val yes = answer.await()
        withContext(Dispatchers.Main) { question = null }
        yes
    }

    fun answer(yes: Boolean) {
        question?.answer?.complete(yes)
    }

    // ---- Running one call --------------------------------------------------------

    private fun run(label: String, call: (Long) -> String, then: (EwrResult) -> Unit) {
        if (busy != null)
            return

        busy = label
        progress = null
        outcome = null
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                if (host.isEmpty())
                    usb.connect()

                val opened = EwrNative.open(storage.database.path, host.ifEmpty { null })
                val handle = opened[1]
                if (handle == 0L) {
                    EwrResult(opened[0].toInt(), "failed", "Could not start EWR.", null)
                } else try {
                    if (opened[0] != 0L)
                        EwrResult(opened[0].toInt(), "failed", EwrNative.lastError(handle), null)
                    else
                        EwrResult.parse(call(handle))
                } finally {
                    EwrNative.close(handle)
                }
            }

            busy = null
            progress = null
            if (!result.ok)
                outcome = result.error.ifEmpty { "Stopped (${result.statusName})." }
            then(result)
        }
    }

    // ---- What the screen asks for ------------------------------------------------

    fun refreshUsb() {
        usbNames = usb.epsonDevices().map { it.productName ?: it.deviceName }
    }

    fun connectUsb() {
        host = ""
        val device = usb.epsonDevices().firstOrNull()
        if (device == null) {
            outcome = "No Epson printer on USB. Connect it with an OTG cable, or search the network."
            return
        }
        if (usb.hasPermission(device))
            detect()
        else
            usb.requestPermission(device) { granted ->
                if (granted) detect() else outcome = "USB permission was not given."
            }
    }

    fun useHost(address: String) {
        host = address.trim()
        detect()
    }

    fun searchNetwork() {
        val wifi = app.applicationContext.getSystemService(WifiManager::class.java)
        val lock = wifi?.createMulticastLock("ewr-discovery")
        lock?.acquire()
        run("Searching the network", { EwrNative.discoverNetwork(it, callbacks) }) { result ->
            lock?.release()
            val found = result.data?.optJSONArray("printers")
            networkPrinters = (0 until (found?.length() ?: 0)).map { i ->
                val p = found!!.getJSONObject(i)
                p.optString("address") to p.optString("reported_model")
            }
            if (result.ok && networkPrinters.isEmpty())
                outcome = "No Epson printer answered on this network."
        }
    }

    fun detect() {
        run("Asking the printer what it is", { handle ->
            if (models.isEmpty()) {
                val list = EwrResult.parse(EwrNative.listModels(handle)).data?.optJSONArray("models")
                val names = (0 until (list?.length() ?: 0)).map { list!!.getJSONObject(it).optString("name") }
                viewModelScope.launch(Dispatchers.Main) { models = names.sorted() }
            }
            EwrNative.detectModel(handle, callbacks)
        }) { result ->
            reported = result.data?.optString("reported_model")?.takeIf { it.isNotEmpty() && it != "null" }
            val entry = result.data?.optString("model")?.takeIf { it.isNotEmpty() && it != "null" }
            if (entry != null)
                model = entry
            if (result.ok && entry == null)
                outcome = "The printer reports \"${reported ?: "nothing"}\", which no database entry matches. " +
                    "Pick the model by hand."
            if (model != null)
                readStatus()
        }
    }

    fun chooseModel(name: String) {
        model = name
        status = null
        info = null
        readStatus()
    }

    fun readStatus() {
        val name = model ?: return
        run("Reading the printer", { EwrNative.readStatus(it, name, callbacks) }) { result ->
            if (result.ok)
                status = result.data
        }
    }

    fun reset(ink: Boolean) {
        val name = model ?: return
        run(if (ink) "Resetting the ink levels" else "Resetting the waste ink pads",
            { EwrNative.reset(it, name, ink, callbacks) }) { result ->
            if (result.ok) {
                outcome = "Done and verified. Turn the printer off and on with its power button."
                readStatus()
            }
        }
    }

    fun restore(file: File) = restoreText(file.readText())

    fun importBackup(uri: Uri) {
        val text = app.contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() } ?: return
        restoreText(text)
    }

    private fun restoreText(text: String) {
        run("Restoring", { EwrNative.restore(it, model, text, callbacks) }) { result ->
            if (result.ok) {
                val unchanged = result.data?.optJSONObject("writes")?.optInt("total", 0) == 0
                outcome = if (unchanged) "Nothing to restore: every byte already holds the backup's value."
                          else "Restored and verified. Turn the printer off and on with its power button."
                readStatus()
            }
        }
    }

    fun headClean(level: String, group: Int, power: Boolean) {
        run(if (power) "Power cleaning" else "Cleaning the print head",
            { EwrNative.headClean(it, model, level, group, power, false, callbacks) }) { result ->
            if (result.ok)
                outcome = "The printer finished after ${result.data?.optInt("seconds")} s. Print a nozzle check to see " +
                    "whether it helped."
        }
    }

    fun nozzleCheck() {
        run("Printing the nozzle check", { EwrNative.nozzleCheck(it, model, 0, callbacks) }) { result ->
            if (result.ok)
                outcome = "The nozzle check is printed."
        }
    }

    fun readInfo() {
        run("Reading the printer's own counters", { EwrNative.readInfo(it, model, callbacks) }) { result ->
            if (result.ok)
                info = result.data
        }
    }

    fun updateDatabase() {
        if (busy != null)
            return
        busy = "Updating the printer database"
        viewModelScope.launch {
            val update = withContext(Dispatchers.IO) { storage.updateDatabase() }
            busy = null
            models = emptyList()
            outcome = when (update) {
                Storage.Update.UPDATED -> "The printer database is updated."
                Storage.Update.UP_TO_DATE -> "The printer database is up to date."
                Storage.Update.FAILED -> "The update could not be fetched; the database on this phone is unchanged."
            }
        }
    }

    override fun onCleared() {
        usb.disconnect()
    }
}
