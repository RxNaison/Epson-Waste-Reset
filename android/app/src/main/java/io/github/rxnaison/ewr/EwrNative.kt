package io.github.rxnaison.ewr

import org.json.JSONObject

/**
 * What a native call hears from EWR while it runs. Every method is called on
 * the thread that made the call; the decisions block it until they return.
 */
interface EwrCallbacks {
    fun onEvent(code: String, message: String, level: Int, stage: Int, index: Int, total: Int, fieldsJson: String)

    /** A gate objects (a printer error, a missing backup...): go ahead anyway? */
    fun decideBlocker(json: String): Boolean

    /** The one question before anything is written or sent. */
    fun confirm(json: String): Boolean

    /** The bytes a write is about to overwrite: true once they are kept. */
    fun saveBackup(json: String): Boolean
}

/** One answer from EWR: docs/c-api.md's status, and the call's JSON. */
data class EwrResult(val status: Int, val statusName: String, val error: String, val data: JSONObject?) {
    val ok: Boolean get() = status == 0

    companion object {
        fun parse(text: String): EwrResult {
            val root = JSONObject(text)
            return EwrResult(
                status = root.getInt("status"),
                statusName = root.optString("status_name"),
                error = root.optString("error"),
                data = root.optJSONObject("data"),
            )
        }
    }
}

/** EWR's C ABI, through the JNI shim in src/main/cpp/ewr_jni.cpp. */
object EwrNative {
    init {
        System.loadLibrary("ewr_jni")
    }

    @JvmStatic external fun abiVersion(): Int
    @JvmStatic external fun version(): String
    @JvmStatic external fun setDataDir(dir: String)
    @JvmStatic external fun setUsbFds(fds: IntArray)
    @JvmStatic external fun databaseCheck(path: String): String

    /** [status, handle]; the handle is 0 when nothing was opened. */
    @JvmStatic external fun open(databasePath: String, host: String?): LongArray
    @JvmStatic external fun close(handle: Long)
    @JvmStatic external fun lastError(handle: Long): String

    @JvmStatic external fun listModels(handle: Long): String
    @JvmStatic external fun detectModel(handle: Long, callbacks: EwrCallbacks?): String
    @JvmStatic external fun discoverNetwork(handle: Long, callbacks: EwrCallbacks?): String
    @JvmStatic external fun readStatus(handle: Long, model: String, callbacks: EwrCallbacks?): String
    @JvmStatic external fun plan(handle: Long, model: String, ink: Boolean): String
    @JvmStatic external fun reset(handle: Long, model: String, ink: Boolean, callbacks: EwrCallbacks?): String
    @JvmStatic external fun restore(handle: Long, model: String?, contents: String, callbacks: EwrCallbacks?): String
    @JvmStatic external fun headClean(
        handle: Long, model: String?, level: String?, group: Int, power: Boolean, printData: Boolean,
        callbacks: EwrCallbacks?,
    ): String
    @JvmStatic external fun nozzleCheck(handle: Long, model: String?, variant: Int, callbacks: EwrCallbacks?): String
    @JvmStatic external fun readInfo(handle: Long, model: String?, callbacks: EwrCallbacks?): String

    const val CLEAN_ALL = 0
    const val CLEAN_BLACK = 1
    const val CLEAN_COLOR = 2
}
