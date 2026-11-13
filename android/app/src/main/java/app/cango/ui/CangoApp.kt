package app.cango.ui

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import app.cango.BleSession
import app.cango.CanFrame
import org.json.JSONObject

/** BLE client. Filters are sent with set_filter because the link is narrow. */
@Composable
fun CangoApp() {
    val context = LocalContext.current
    var link by remember { mutableStateOf("idle") }
    var profile by remember { mutableStateOf("dealer") }
    var armed by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf(false) }
    var screen by remember { mutableIntStateOf(0) }
    var filterId by remember { mutableStateOf("100") }
    var filterMask by remember { mutableStateOf("7FF") }
    var sendId by remember { mutableStateOf("123") }
    var sendData by remember { mutableStateOf("") }
    var quiet by remember { mutableStateOf("5") }
    val frames = remember { mutableStateListOf<CanFrame>() }
    val session = remember {
        BleSession(
            context.applicationContext,
            onStatus = { link = it },
            onFrames = { batch ->
                frames.addAll(batch)
                while (frames.size > 80) frames.removeAt(0)
            },
            onJson = { line ->
                val obj = JSONObject(line)
                if (obj.optString("op") == "bus_status") {
                    armed = obj.optBoolean("armed")
                    profile = obj.optString("profile", profile)
                    link = obj.optString("state") + " " + obj.optString("mode")
                } else if (obj.optString("op") == "arm_tx" && obj.optBoolean("ok")) {
                    armed = true
                } else if (obj.optString("op") == "disarm") {
                    armed = false
                }
            },
        )
    }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        if (granted.values.all { it }) session.startScan() else link = "permission denied"
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp)) {
        Text("CANGO  $link")
        Text("Profile $profile. Mode changes with the BOOT button.")
        Row {
            Button(onClick = {
                val needed = if (Build.VERSION.SDK_INT >= 31) {
                    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
                } else {
                    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
                }
                permission.launch(needed)
            }) { Text("Scan") }
            Button(onClick = { screen = 0 }) { Text("Live") }
            Button(onClick = { screen = 1 }) { Text("Capture") }
            Button(onClick = { screen = 2 }) { Text("Inject") }
            Button(onClick = { screen = 3 }) { Text("Logs") }
        }
        when (screen) {
            0 -> {
                OutlinedTextField(filterId, { filterId = it }, label = { Text("ID hex") })
                OutlinedTextField(filterMask, { filterMask = it }, label = { Text("Mask hex") })
                Button(onClick = {
                    val id = filterId.toIntOrNull(16) ?: return@Button
                    val mask = filterMask.toIntOrNull(16) ?: return@Button
                    session.send("""{"op":"set_filter","active":true,"id":$id,"mask":$mask,"extended":"any"}""")
                }) { Text("Apply filter") }
                frames.takeLast(12).forEach { frame ->
                    Text("%X  %s".format(frame.id, frame.data.joinToString("") { "%02X".format(it) }))
                }
            }
            1 -> {
                Text("Default is dealer. This does not start WiFi or stop BLE.")
                OutlinedTextField(profile, { profile = it }, label = { Text("dealer, startup, or both") })
                OutlinedTextField(quiet, { quiet = it }, label = { Text("Dealer quiet s") })
                Button(onClick = {
                    if (profile != "dealer" && profile != "startup" && profile != "both") return@Button
                    session.send("""{"op":"set_capture","profile":"$profile","dealer_quiet_s":${quiet.toIntOrNull() ?: 5}}""")
                }) { Text("Save profile") }
            }
            2 -> {
                Row {
                    Checkbox(confirm, { confirm = it })
                    Text("I intend to transmit on this bus")
                }
                Button(enabled = confirm && !armed, onClick = { session.send("""{"op":"arm_tx","confirm":true}""") }) { Text("Arm") }
                Button(enabled = armed, onClick = { session.send("""{"op":"disarm"}""") }) { Text("Disarm") }
                OutlinedTextField(sendId, { sendId = it }, label = { Text("ID hex") })
                OutlinedTextField(sendData, { sendData = it }, label = { Text("Data hex") })
                Button(enabled = armed, onClick = {
                    val id = sendId.toIntOrNull(16) ?: return@Button
                    val data = sendData.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' }
                    session.send("""{"op":"send_frame","id":$id,"data":"$data"}""")
                }) { Text("Send frame") }
                Text("Empty allow list rejects every id. Arm lasts 60 seconds.")
            }
            else -> {
                Text("Logs stay on the SD card. Long-press BOOT until the LED flashes green, join CANGO-XXXX, and download from the page. BLE does not carry log files.")
                Button(onClick = { session.send("""{"op":"list_logs"}""") }) { Text("List names") }
            }
        }
        Button(enabled = false, onClick = {}) { Text("Find modules") }
        Button(enabled = false, onClick = {}) { Text("Run action") }
    }
}
