package app.cango.ui

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.Alignment
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
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

/** Same charcoal as the system device-details screen. */
const val CangoGrey: Int = 0xFF151A1E.toInt()

@Composable
private fun fieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = Color.White,
    unfocusedTextColor = Color.White,
    disabledTextColor = Color.White,
    focusedLabelColor = Color.White,
    unfocusedLabelColor = Color.White,
    disabledLabelColor = Color.White,
    cursorColor = Color.White,
    focusedBorderColor = Color.White,
    unfocusedBorderColor = Color(0xFF8E98A0),
)

@Composable
private fun WhiteButton(
    onClick: () -> Unit,
    enabled: Boolean = true,
    content: @Composable RowScope.() -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = ButtonDefaults.buttonColors(
            containerColor = Color(0xFF31576D),
            contentColor = Color.White,
            disabledContainerColor = Color(0xFF262D32),
            disabledContentColor = Color.White,
        ),
        content = content,
    )
}

private val LedOff = Color(0xFF3A4450)
private val LedBlue = Color(0xFF3D7BFF)
private val LedGreen = Color(0xFF3DDC6A)
private val LedPurple = Color(0xFFC45CFF)

/** Dealer, BLE, and WiFi, in the same colors as the board LED. */
@Composable
private fun LedStatus(mode: String, capture: String) {
    val dealer = when {
        mode == "capture" && capture == "complete" -> Color.White
        mode == "capture" -> LedPurple
        else -> LedOff
    }
    Row(
        Modifier.fillMaxWidth().padding(bottom = 8.dp),
        horizontalArrangement = Arrangement.End,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Glyph("Dealer", dealer) { dealerGlyph(it) }
        Glyph("BLE", if (mode == "ble") LedBlue else LedOff) { bleGlyph(it) }
        Glyph("WiFi", if (mode == "wifi") LedGreen else LedOff) { wifiGlyph(it) }
    }
}

@Composable
private fun Glyph(name: String, color: Color, draw: DrawScope.(Color) -> Unit) {
    Canvas(
        Modifier
            .padding(start = 14.dp)
            .size(28.dp)
            .semantics { contentDescription = name },
    ) {
        draw(color)
    }
}

private fun DrawScope.dealerGlyph(color: Color) {
    val s = size.minDimension
    val stroke = s * 0.09f
    val left = s * 0.42f
    val top = s * 0.16f
    val height = s * 0.68f
    drawLine(color, Offset(left, top), Offset(left, top + height), stroke, StrokeCap.Round)
    drawArc(
        color,
        startAngle = -90f,
        sweepAngle = 180f,
        useCenter = false,
        topLeft = Offset(left - height / 2f, top),
        size = Size(height, height),
        style = Stroke(width = stroke, cap = StrokeCap.Round),
    )
}

private fun DrawScope.bleGlyph(color: Color) {
    val s = size.minDimension
    val w = s * 0.09f
    val x = s * 0.50f
    val top = s * 0.08f
    val upper = s * 0.32f
    val mid = s * 0.50f
    val lower = s * 0.68f
    val bot = s * 0.92f
    val left = s * 0.22f
    val right = s * 0.78f
    fun line(a: Offset, b: Offset) = drawLine(color, a, b, w, StrokeCap.Round)
    line(Offset(x, top), Offset(x, bot))
    line(Offset(x, top), Offset(right, upper))
    line(Offset(right, upper), Offset(x, mid))
    line(Offset(x, mid), Offset(right, lower))
    line(Offset(right, lower), Offset(x, bot))
    line(Offset(left, upper), Offset(right, lower))
    line(Offset(left, lower), Offset(right, upper))
}

private fun DrawScope.wifiGlyph(color: Color) {
    val s = size.minDimension
    val stroke = s * 0.09f
    val cx = s * 0.50f
    val base = s * 0.78f
    drawCircle(color, radius = stroke * 0.8f, center = Offset(cx, base))
    for (r in floatArrayOf(s * 0.22f, s * 0.36f, s * 0.50f)) {
        drawArc(
            color = color,
            startAngle = 210f,
            sweepAngle = 120f,
            useCenter = false,
            topLeft = Offset(cx - r, base - r),
            size = Size(r * 2f, r * 2f),
            style = Stroke(width = stroke, cap = StrokeCap.Round),
        )
    }
}

private val CangoScheme = darkColorScheme(
    background = Color(CangoGrey),
    surface = Color(CangoGrey),
    surfaceVariant = Color(0xFF262D32),
    onBackground = Color.White,
    onSurface = Color.White,
    onSurfaceVariant = Color.White,
    primary = Color(0xFF31576D),
    onPrimary = Color.White,
    outline = Color(0xFF8E98A0),
)

/** BLE client. Filters are sent with set_filter because the link is narrow. */
@Composable
fun CangoApp() {
    val context = LocalContext.current
    var mode by remember { mutableStateOf("") }
    var capture by remember { mutableStateOf("idle") }
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
            onStatus = { text ->
                if (text == "ready" || text == "connected") mode = "ble"
            },
            onFrames = { batch ->
                frames.addAll(batch)
                while (frames.size > 80) frames.removeAt(0)
            },
            onJson = { line ->
                val obj = JSONObject(line)
                if (obj.optString("op") == "bus_status") {
                    armed = obj.optBoolean("armed")
                    profile = obj.optString("profile", profile)
                    capture = obj.optString("capture", capture)
                    val reported = obj.optString("mode")
                    if (reported.isNotEmpty()) mode = reported
                } else if (obj.optString("op") == "arm_tx" && obj.optBoolean("ok")) {
                    armed = true
                } else if (obj.optString("op") == "disarm") {
                    armed = false
                }
            },
        )
    }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        if (granted.values.all { it }) session.startScan()
    }
    MaterialTheme(colorScheme = CangoScheme) {
    Surface(Modifier.fillMaxSize(), color = CangoScheme.background, contentColor = Color.White) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp)) {
        LedStatus(mode, capture)
        Row {
            WhiteButton(onClick = {
                val needed = if (Build.VERSION.SDK_INT >= 31) {
                    arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
                } else {
                    arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
                }
                permission.launch(needed)
            }) { Text("Scan", color = Color.White) }
            WhiteButton(onClick = { screen = 0 }) { Text("Live", color = Color.White) }
            WhiteButton(onClick = { screen = 1 }) { Text("Capture", color = Color.White) }
            WhiteButton(onClick = { screen = 2 }) { Text("Inject", color = Color.White) }
            WhiteButton(onClick = { screen = 3 }) { Text("Logs", color = Color.White) }
        }
        when (screen) {
            0 -> {
                OutlinedTextField(filterId, { filterId = it }, label = { Text("ID hex", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(filterMask, { filterMask = it }, label = { Text("Mask hex", color = Color.White) }, colors = fieldColors())
                WhiteButton(onClick = {
                    val id = filterId.toIntOrNull(16) ?: return@WhiteButton
                    val mask = filterMask.toIntOrNull(16) ?: return@WhiteButton
                    session.send("""{"op":"set_filter","active":true,"id":$id,"mask":$mask,"extended":"any"}""")
                }) { Text("Apply filter", color = Color.White) }
                frames.takeLast(12).forEach { frame ->
                    Text("%X  %s".format(frame.id, frame.data.joinToString("") { "%02X".format(it) }), color = Color.White)
                }
            }
            1 -> {
                Text("Default is dealer. This does not start WiFi or stop BLE.", color = Color.White)
                OutlinedTextField(profile, { profile = it }, label = { Text("dealer, startup, or both", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(quiet, { quiet = it }, label = { Text("Dealer quiet s", color = Color.White) }, colors = fieldColors())
                WhiteButton(onClick = {
                    if (profile != "dealer" && profile != "startup" && profile != "both") return@WhiteButton
                    session.send("""{"op":"set_capture","profile":"$profile","dealer_quiet_s":${quiet.toIntOrNull() ?: 5}}""")
                }) { Text("Save profile", color = Color.White) }
            }
            2 -> {
                Row {
                    Checkbox(confirm, { confirm = it })
                    Text("I intend to transmit on this bus", color = Color.White)
                }
                WhiteButton(enabled = confirm && !armed, onClick = { session.send("""{"op":"arm_tx","confirm":true}""") }) { Text("Arm", color = Color.White) }
                WhiteButton(enabled = armed, onClick = { session.send("""{"op":"disarm"}""") }) { Text("Disarm", color = Color.White) }
                OutlinedTextField(sendId, { sendId = it }, label = { Text("ID hex", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(sendData, { sendData = it }, label = { Text("Data hex", color = Color.White) }, colors = fieldColors())
                WhiteButton(enabled = armed, onClick = {
                    val id = sendId.toIntOrNull(16) ?: return@WhiteButton
                    val data = sendData.filter { it.isDigit() || it.lowercaseChar() in 'a'..'f' }
                    session.send("""{"op":"send_frame","id":$id,"data":"$data"}""")
                }) { Text("Send frame", color = Color.White) }
                Text("Empty allow list rejects every id. Arm lasts 60 seconds.", color = Color.White)
            }
            else -> {
                Text("Logs stay on the SD card. Long-press BOOT until the LED flashes green, join CANGO-XXXX, and download from the page. BLE does not carry log files.", color = Color.White)
                WhiteButton(onClick = { session.send("""{"op":"list_logs"}""") }) { Text("List names", color = Color.White) }
            }
        }
        WhiteButton(enabled = false, onClick = {}) { Text("Find modules", color = Color.White) }
        WhiteButton(enabled = false, onClick = {}) { Text("Run action", color = Color.White) }
    }
    }
    }
}
