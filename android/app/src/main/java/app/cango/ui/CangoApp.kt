package app.cango.ui

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import app.cango.BleSession
import kotlinx.coroutines.delay
import org.json.JSONObject

/** Same charcoal as the system device-details screen. */
const val CangoGrey: Int = 0xFF151A1E.toInt()

@Composable
internal fun fieldColors() = OutlinedTextFieldDefaults.colors(
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
internal fun WhiteButton(
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

/** Capture, BLE, and WiFi, in the same colors as the board LED. */
@Composable
private fun LedStatus(mode: String, capture: String, linked: Boolean) {
    val logging = mode == "capture" && capture == "running"
    val bleWait = mode == "ble" && !linked
    var on by remember { mutableStateOf(true) }
    LaunchedEffect(logging || bleWait) {
        if (!logging && !bleWait) {
            on = true
            return@LaunchedEffect
        }
        while (true) {
            on = !on
            delay(150)
        }
    }
    val captureColor = when {
        mode != "capture" -> LedOff
        logging && !on -> LedOff
        else -> LedPurple
    }
    val bleColor = when {
        mode != "ble" -> LedOff
        linked -> LedBlue
        on -> LedBlue
        else -> LedOff
    }
    Row(
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Glyph("Capture", captureColor) { dealerGlyph(it) }
        Glyph("BLE", bleColor) { bleGlyph(it) }
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

private fun jsonEscape(text: String): String = text.replace("\\", "\\\\").replace("\"", "\\\"")

@Composable
private fun ConfigMark(open: Boolean, onClick: () -> Unit) {
    Canvas(
        Modifier
            .size(40.dp)
            .clickable(onClick = onClick)
            .semantics { contentDescription = "Config" },
    ) {
        val color = if (open) Color.White else Color(0xFF8E98A0)
        val s = size.minDimension
        val left = s * 0.18f
        val right = s * 0.82f
        val stroke = s * 0.08f
        listOf(0.30f, 0.50f, 0.70f).forEach { y ->
            drawLine(color, Offset(left, s * y), Offset(right, s * y), stroke, StrokeCap.Round)
        }
    }
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
    var linked by remember { mutableStateOf(false) }
    var heard by remember { mutableStateOf(0L) }
    var project by remember { mutableStateOf("generic") }
    var ssid0 by remember { mutableStateOf("") }
    var pass0 by remember { mutableStateOf("") }
    var ssid1 by remember { mutableStateOf("") }
    var pass1 by remember { mutableStateOf("") }
    var picker by remember { mutableStateOf(false) }
    var armed by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf(false) }
    var screen by remember { mutableIntStateOf(0) }
    var menu by remember { mutableStateOf(false) }
    var help by remember { mutableStateOf(false) }
    var sendId by remember { mutableStateOf("123") }
    var sendData by remember { mutableStateOf("") }
    val sniff = remember { SniffModel() }
    val files = remember { CardModel() }
    val session = remember {
        BleSession(
            context.applicationContext,
            onStatus = { text ->
                when {
                    text == "ready" || text == "connected" -> {
                        mode = "ble"
                        linked = true
                        heard = System.currentTimeMillis()
                    }
                    text.startsWith("disconnected") || text.startsWith("discover failed") || text == "service missing" -> {
                        linked = false
                    }
                }
            },
            onFrames = { batch -> sniff.ingest(batch) },
            onJson = { line ->
                val obj = JSONObject(line)
                if (obj.optString("op") == "bus_status") {
                    armed = obj.optBoolean("armed")
                    project = obj.optString("project", project)
                    capture = obj.optString("capture", capture)
                    val reported = obj.optString("mode")
                    if (reported.isNotEmpty()) mode = reported
                    heard = System.currentTimeMillis()
                    linked = true
                } else if (obj.optString("op") == "list_dir") {
                    files.apply(obj)
                } else if (obj.optString("op") == "delete_path" && obj.optBoolean("ok")) {
                    files.refresh()
                } else if (obj.optString("op") == "get_wifi") {
                    val entries = obj.optJSONArray("entries")
                    if (entries != null) {
                        for (i in 0 until entries.length()) {
                            val entry = entries.getJSONObject(i)
                            when (entry.optInt("slot")) {
                                0 -> {
                                    ssid0 = entry.optString("ssid")
                                    pass0 = entry.optString("password")
                                }
                                1 -> {
                                    ssid1 = entry.optString("ssid")
                                    pass1 = entry.optString("password")
                                }
                            }
                        }
                    }
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
    LaunchedEffect(linked) {
        if (!linked) return@LaunchedEffect
        while (true) {
            delay(1000)
            if (System.currentTimeMillis() - heard > 3000) {
                linked = false
                return@LaunchedEffect
            }
        }
    }
    LaunchedEffect(menu, linked) {
        if (menu && linked) session.send("""{"op":"get_wifi"}""")
    }
    LaunchedEffect(linked, sniff.rev) {
        if (linked) session.send(sniff.json())
    }
    val cardPath = files.path
    val cardSkip = files.skip
    val cardGen = files.generation
    LaunchedEffect(linked, menu, screen, cardPath, cardSkip, cardGen) {
        if (linked && !menu && screen == 2) session.send(files.request())
    }
    MaterialTheme(colorScheme = CangoScheme) {
    Surface(Modifier.fillMaxSize(), color = CangoScheme.background, contentColor = Color.White) {
    Box(Modifier.fillMaxSize()) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(12.dp).padding(bottom = 72.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            ConfigMark(open = menu, onClick = { menu = !menu })
            Text(
                "?",
                color = if (help) Color.White else Color(0xFF8E98A0),
                modifier = Modifier
                    .clickable { help = !help }
                    .padding(8.dp)
                    .semantics { contentDescription = "Help" },
            )
            Spacer(Modifier.width(8.dp))
            Spacer(Modifier.weight(1f))
            LedStatus(mode, capture, linked)
        }
        Row {
            WhiteButton(onClick = { menu = false; screen = 0 }) { Text("Live", color = Color.White) }
            WhiteButton(onClick = { menu = false; screen = 1 }) { Text("Inject", color = Color.White) }
            WhiteButton(onClick = { menu = false; screen = 2 }) { Text("Files", color = Color.White) }
        }
        when (if (menu) 3 else screen) {
            0 -> SniffView(sniff)
            1 -> {
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
            }
            2 -> FilesView(files) { session.send(it) }
            3 -> {
                WhiteButton(onClick = {
                    val needed = if (Build.VERSION.SDK_INT >= 31) {
                        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
                    } else {
                        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
                    }
                    permission.launch(needed)
                }) { Text("Scan", color = Color.White) }
                OutlinedTextField(ssid0, { ssid0 = it }, label = { Text("WiFi 1", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(pass0, { pass0 = it }, label = { Text("Password 1", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(ssid1, { ssid1 = it }, label = { Text("WiFi 2", color = Color.White) }, colors = fieldColors())
                OutlinedTextField(pass1, { pass1 = it }, label = { Text("Password 2", color = Color.White) }, colors = fieldColors())
                WhiteButton(enabled = linked, onClick = {
                    session.send("""{"op":"set_wifi","slot":0,"ssid":"${jsonEscape(ssid0)}","password":"${jsonEscape(pass0)}"}""")
                    session.send("""{"op":"set_wifi","slot":1,"ssid":"${jsonEscape(ssid1)}","password":"${jsonEscape(pass1)}"}""")
                }) { Text("Save WiFi", color = Color.White) }
                WhiteButton(enabled = false, onClick = {}) { Text("Find modules", color = Color.White) }
            }
        }
    }
    FloatingActionButton(
        onClick = { picker = true },
        modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp).semantics {
            contentDescription = if (project == "zero-xb") "Vehicle Zero XB" else "Vehicle"
        },
        containerColor = Color(0xFF31576D),
        contentColor = Color.White,
    ) { Text("+", color = Color.White) }
    }
    }
    if (help) {
        AlertDialog(
            onDismissRequest = { help = false },
            containerColor = Color(CangoGrey),
            text = {
                Column {
                    Text("Row — one id, newest bytes", color = Color.White)
                    Text("Check — hide that id here", color = Color.White)
                    Text("Notch — freeze moving bits", color = Color.White)
                    Text("Hold — keep quiet ids", color = Color.White)
                    Text("Files — open a folder or drop a row", color = Color.White)
                }
            },
            confirmButton = {
                TextButton(
                    onClick = { help = false },
                    colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                ) { Text("Close") }
            },
        )
    }
    if (picker) {
        AlertDialog(
            onDismissRequest = { picker = false },
            containerColor = Color(CangoGrey),
            title = { Text("Vehicle", color = Color.White) },
            text = {
                TextButton(
                    onClick = {
                        session.send("""{"op":"apply_project","id":"zero-xb"}""")
                        project = "zero-xb"
                        picker = false
                    },
                    colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                ) { Text("Zero XB") }
            },
            confirmButton = {
                TextButton(
                    onClick = { picker = false },
                    colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                ) { Text("Close") }
            },
        )
    }
    }
}
