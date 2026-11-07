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

        // Staged and renamed, as a download is: a full disk keeps the database
        // in use, and the next start tries again.
        val staged = File(context.filesDir, "database.json.seed")
        try {
            context.assets.open("database.json").use { input ->
                staged.outputStream().use { output -> input.copyTo(output) }
            }
            if (staged.renameTo(database))
                prefs.edit().putInt("seeded_version", BuildConfig.VERSION_CODE).apply()
        } catch (_: Exception) {
        } finally {
            staged.delete()
        }
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

            // A rename, not a copy: a copy cut short would leave a truncated
            // database, and the next start, this version already seeded, keeps it.
            return if (staged.renameTo(database)) Update.UPDATED else Update.FAILED
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

        // Whole or not at all: a backup cut short must not sit in the list a
        // restore is picked from.
        val staged = File(backupsDir, "${file.name}.tmp")
        try {
            staged.writeText(json)
            if (staged.renameTo(file)) file else null
        } finally {
            staged.delete()
        }
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
