package io.github.rxnaison.ewr

import android.content.Context
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/** The app's own files: the database it runs on, and the backups it keeps. */
class Storage(private val context: Context) {

    enum class Update { UPDATED, UP_TO_DATE, FAILED }

    val database: File get() = File(context.filesDir, "database.json")
    val backupsDir: File get() = File(context.filesDir, "backups").apply { mkdirs() }

    /**
     * The APK carries the database of its release. It replaces the one in
     * use on first run and after an app update; a download in between wins
     * until the next update.
     */
    fun seedDatabase() {
        val prefs = context.getSharedPreferences("ewr", Context.MODE_PRIVATE)
        val seeded = prefs.getInt("seeded_version", -1)
        if (database.exists() && seeded == BuildConfig.VERSION_CODE)
            return

        context.assets.open("database.json").use { input ->
            database.outputStream().use { output -> input.copyTo(output) }
        }
        prefs.edit().putInt("seeded_version", BuildConfig.VERSION_CODE).apply()
    }

    /**
     * The desktop updater's rule: a download is used only once EWR itself
     * has loaded it. Blocking; call it off the main thread.
     */
    fun updateDatabase(): Update {
        val staged = File(context.filesDir, "database.json.staged")
        try {
            val connection = URL(DATABASE_URL).openConnection() as HttpURLConnection
            connection.connectTimeout = 10_000
            connection.readTimeout = 30_000
            connection.setRequestProperty("User-Agent", "EWR-Android/${BuildConfig.VERSION_NAME}")
            try {
                if (connection.responseCode != HttpURLConnection.HTTP_OK)
                    return Update.FAILED
                connection.inputStream.use { input -> staged.outputStream().use { input.copyTo(it) } }
            } finally {
                connection.disconnect()
            }

            if (!EwrResult.parse(EwrNative.databaseCheck(staged.path)).ok)
                return Update.FAILED

            if (database.exists() && staged.readBytes().contentEquals(database.readBytes()))
                return Update.UP_TO_DATE

            staged.copyTo(database, overwrite = true)
            return Update.UPDATED
        } catch (_: Exception) {
            return Update.FAILED
        } finally {
            staged.delete()
        }
    }

    /** Keeps a backup EWR hands over before a write; null if it could not. */
    fun saveBackup(json: String): File? = try {
        val backup = JSONObject(json)
        val model = backup.optString("model", "unknown")
        val serial = backup.optJSONObject("printer")?.optString("serial")?.takeIf { it.isNotEmpty() && it != "null" }
            ?: "unknown"
        val stamp = backup.optString("created").filter { it.isLetterOrDigit() }.ifEmpty { "undated" }
        val stem = listOf(model, serial, stamp).joinToString("_") { part ->
            part.map { if (it.isLetterOrDigit() || it == '-') it else '_' }.joinToString("")
        }

        var file = File(backupsDir, "$stem.json")
        var n = 2
        while (file.exists())
            file = File(backupsDir, "${stem}_${n++}.json")

        file.writeText(json)
        file
    } catch (_: Exception) {
        null
    }

    fun backups(): List<File> =
        backupsDir.listFiles { file -> file.extension == "json" }?.sortedByDescending { it.lastModified() }
            ?: emptyList()

    companion object {
        const val DATABASE_URL = "https://raw.githubusercontent.com/RxNaison/Epson-Waste-Reset/main/database.json"
    }
}
