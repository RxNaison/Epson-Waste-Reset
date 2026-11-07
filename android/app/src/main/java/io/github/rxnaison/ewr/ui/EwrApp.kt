package io.github.rxnaison.ewr.ui

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import io.github.rxnaison.ewr.EwrNative
import io.github.rxnaison.ewr.EwrViewModel
import org.json.JSONObject

@Composable
fun EwrTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) darkColorScheme() else lightColorScheme(), content = content)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EwrApp(vm: EwrViewModel) {
    Scaffold(topBar = { TopAppBar(title = { Text("EWR ${EwrNative.version()}") }) }) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize().padding(padding),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item { BusyLine(vm) }
            item { PrinterCard(vm) }
            if (vm.model != null) {
                item { StatusCard(vm) }
                item { ResetCard(vm) }
                item { MaintenanceCard(vm) }
            }
            item { BackupsCard(vm) }
            if (vm.model != null)
                item { InfoCard(vm) }
            item { LogCard(vm) }
        }
    }

    vm.question?.let { QuestionDialog(it, vm::answer) }
}

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

@Composable
private fun BusyLine(vm: EwrViewModel) {
    val busy = vm.busy
    if (busy != null) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("$busy...")
            val progress = vm.progress
            if (progress != null)
                LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
            else
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
    } else {
        vm.outcome?.let { Text(it, style = MaterialTheme.typography.bodyLarge) }
    }
}

@Composable
private fun PrinterCard(vm: EwrViewModel) {
    var address by remember { mutableStateOf("") }
    var picking by remember { mutableStateOf(false) }

    Section("Printer") {
        val where = if (vm.host.isEmpty()) "USB" else vm.host
        Text("Connection: $where")
        vm.reported?.let { Text("The printer reports: $it") }
        Text("Database entry: ${vm.model ?: "none chosen"}", fontWeight = FontWeight.Medium)

        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = vm::connectUsb, enabled = vm.busy == null) { Text("USB printer") }
            OutlinedButton(onClick = vm::searchNetwork, enabled = vm.busy == null) { Text("Search Wi-Fi") }
            OutlinedButton(onClick = { picking = true }, enabled = vm.models.isNotEmpty()) { Text("Choose model") }
        }

        if (vm.usbNames.isEmpty() && vm.host.isEmpty())
            Text("No Epson printer on USB. Connect one with an OTG cable, or use Wi-Fi.",
                 style = MaterialTheme.typography.bodySmall)

        vm.networkPrinters.forEach { (ip, name) ->
            TextButton(onClick = { vm.useHost(ip) }) { Text("$name at $ip") }
        }

        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            OutlinedTextField(
                value = address, onValueChange = { address = it }, singleLine = true,
                label = { Text("Printer address") }, modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            OutlinedButton(onClick = { vm.useHost(address) }, enabled = address.isNotBlank() && vm.busy == null) {
                Text("Use")
            }
        }

        OutlinedButton(onClick = vm::updateDatabase, enabled = vm.busy == null) { Text("Update the printer database") }
    }

    if (picking)
        ModelPicker(vm.models, onPick = { picking = false; vm.chooseModel(it) }, onDismiss = { picking = false })
}

@Composable
private fun ModelPicker(models: List<String>, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    var query by remember { mutableStateOf("") }
    val shown = models.filter { it.contains(query, ignoreCase = true) }.take(60)

    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } },
        title = { Text("Choose the model") },
        text = {
            Column {
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true,
                                  label = { Text("Search") }, modifier = Modifier.fillMaxWidth())
                LazyColumn(modifier = Modifier.heightIn(max = 360.dp)) {
                    items(shown) { name -> TextButton(onClick = { onPick(name) }) { Text(name) } }
                }
            }
        },
    )
}

@Composable
private fun Gauge(label: String, percent: Int?, note: String = "") {
    Column {
        Text(if (percent != null) "$label  $percent%$note" else "$label$note", style = MaterialTheme.typography.bodyMedium)
        if (percent != null)
            LinearProgressIndicator(progress = { (percent.coerceIn(0, 100)) / 100f }, modifier = Modifier.fillMaxWidth())
    }
}

@Composable
private fun StatusCard(vm: EwrViewModel) {
    Section("Status") {
        val status = vm.status
        val printer = status?.optJSONObject("printer")
        if (printer == null) {
            Text("Not read yet.")
        } else {
            val error = printer.optString("error").takeIf { !printer.isNull("error") }
            Text("State: ${printer.optString("state")}${if (error != null) " - $error" else ""}",
                 fontWeight = FontWeight.Medium)
            val inks = printer.optJSONArray("inks")
            for (i in 0 until (inks?.length() ?: 0)) {
                val ink = inks!!.getJSONObject(i)
                Gauge(ink.optString("color"), if (ink.isNull("level")) null else ink.getInt("level"),
                      "  ${ink.optString("status")}")
            }
            val pads = status.optJSONArray("pads")
            if ((pads?.length() ?: 0) > 0)
                Text("Waste ink pads", style = MaterialTheme.typography.titleSmall)
            for (i in 0 until (pads?.length() ?: 0)) {
                val pad = pads!!.getJSONObject(i)
                Gauge(pad.optString("name"), if (pad.isNull("percent")) null else pad.getInt("percent"))
            }
        }
        OutlinedButton(onClick = vm::readStatus, enabled = vm.busy == null) { Text("Read again") }
    }
}

@Composable
private fun ResetCard(vm: EwrViewModel) {
    Section("Reset") {
        Text("Reads the counters, asks once, writes, and reads them back. A backup of every byte it " +
             "overwrites is kept first.", style = MaterialTheme.typography.bodySmall)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.reset(ink = false) }, enabled = vm.busy == null) { Text("Reset waste ink pads") }
            OutlinedButton(onClick = { vm.reset(ink = true) }, enabled = vm.busy == null) { Text("Reset ink levels") }
        }
        Text("Afterwards, turn the printer off and on with its power button.", style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun MaintenanceCard(vm: EwrViewModel) {
    var level by remember { mutableStateOf("normal") }
    var group by remember { mutableStateOf(EwrNative.CLEAN_ALL) }

    Section("Maintenance") {
        Text("Nothing here writes the printer's memory, but a cleaning uses ink and fills the waste pads.",
             style = MaterialTheme.typography.bodySmall)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("gentle", "normal", "strong").forEach { name ->
                FilterChip(selected = level == name, onClick = { level = name }, label = { Text(name) })
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("all" to EwrNative.CLEAN_ALL, "black" to EwrNative.CLEAN_BLACK, "color" to EwrNative.CLEAN_COLOR)
                .forEach { (name, value) ->
                    FilterChip(selected = group == value, onClick = { group = value }, label = { Text(name) })
                }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.headClean(level, group, power = false) }, enabled = vm.busy == null) {
                Text("Head cleaning")
            }
            OutlinedButton(onClick = vm::nozzleCheck, enabled = vm.busy == null) { Text("Nozzle check") }
            OutlinedButton(onClick = { vm.headClean("normal", group, power = true) }, enabled = vm.busy == null) {
                Text("Power cleaning")
            }
        }
    }
}

@Composable
private fun BackupsCard(vm: EwrViewModel) {
    val context = LocalContext.current
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importBackup(uri)
    }

    Section("Backups") {
        if (vm.backups.isEmpty())
            Text("None yet. EWR keeps one before every write.", style = MaterialTheme.typography.bodySmall)

        vm.backups.take(12).forEach { file ->
            Column {
                Text(file.name, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { vm.restore(file) }, enabled = vm.busy == null) { Text("Restore") }
                    TextButton(onClick = {
                        val uri = FileProvider.getUriForFile(context, "${context.packageName}.files", file)
                        val share = Intent(Intent.ACTION_SEND).setType("application/json")
                            .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        context.startActivity(Intent.createChooser(share, file.name))
                    }) { Text("Share") }
                }
            }
        }

        OutlinedButton(onClick = { importer.launch(arrayOf("application/json", "text/plain", "*/*")) },
                       enabled = vm.busy == null) {
            Text("Restore from a file")
        }
    }
}

@Composable
private fun InfoCard(vm: EwrViewModel) {
    Section("Printer info") {
        val info = vm.info
        if (info == null) {
            Text("Firmware, serial number and the printer's own counters.", style = MaterialTheme.typography.bodySmall)
        } else {
            Text("Firmware: ${info.optString("firmware").takeIf { !info.isNull("firmware") } ?: "not reported"}")
            Text("Serial: ${info.optString("serial").takeIf { !info.isNull("serial") } ?: "not reported"}")
            val counters = info.optJSONArray("counters")
            for (i in 0 until (counters?.length() ?: 0)) {
                val c = counters!!.getJSONObject(i)
                val label = c.optString("key").replace("_counter", "").replace('_', ' ')
                    .replaceFirstChar { it.uppercase() }
                Text("$label: ${if (c.isNull("value")) "no reply" else c.getLong("value").toString()}",
                     style = MaterialTheme.typography.bodySmall)
            }
            val fatal = info.optJSONArray("fatal_errors")
            val logged = (0 until (fatal?.length() ?: 0)).map { fatal!!.getJSONObject(it) }
                .filter { !it.isNull("code") && it.getInt("code") > 0 }
            if (logged.isNotEmpty())
                Text("Last fatal errors: " + logged.joinToString { "%s 0x%02X".format(it.optString("kind"), it.getInt("code")) })
        }
        OutlinedButton(onClick = vm::readInfo, enabled = vm.busy == null) { Text("Read") }
    }
}

@Composable
private fun LogCard(vm: EwrViewModel) {
    var open by remember { mutableStateOf(false) }
    Section("What EWR did") {
        TextButton(onClick = { open = !open }) { Text(if (open) "Hide" else "Show (${vm.log.size})") }
        if (open)
            vm.log.takeLast(80).forEach { Text(it, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
    }
}

/** No answer is a no: dismissing the dialog declines, and the write never starts. */
@Composable
private fun QuestionDialog(question: EwrViewModel.Question, answer: (Boolean) -> Unit) {
    var typed by remember(question) { mutableStateOf("") }
    val word = (question as? EwrViewModel.Question.Confirm)?.typedWord
    val gate = question is EwrViewModel.Question.Gate

    AlertDialog(
        onDismissRequest = { answer(false) },
        title = { Text(question.title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(question.text)
                if (word != null)
                    OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true,
                                      label = { Text("Type '$word' to go ahead") })
            }
        },
        confirmButton = {
            TextButton(onClick = { answer(true) }, enabled = word == null || typed.trim().lowercase() == word) {
                Text(if (gate) "Go ahead anyway" else "Yes")
            }
        },
        dismissButton = { TextButton(onClick = { answer(false) }) { Text(if (gate) "Stop" else "No") } },
    )
}
