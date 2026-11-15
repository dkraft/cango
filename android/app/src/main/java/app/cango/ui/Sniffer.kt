package app.cango.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.cango.CanFrame
import kotlinx.coroutines.delay

private const val HeardMax = 64
private const val FilterMax = 32
private val Up = Color(0xFF3DDC6A)
private val Down = Color(0xFFE25B5B)
private val Dim = Color(0xFF8E98A0)
private val ByteBg = Color(0xFF151A1E)

private class SniffRow(
    val id: Long,
    val ext: Boolean,
    val bytes: IntArray,
    val prev: IntArray,
    val changedAt: LongArray,
    val notch: IntArray,
    var lastMs: Long,
)

class SniffModel {
    var rev by mutableIntStateOf(0)
    var generation by mutableIntStateOf(0)
    var modeAll by mutableStateOf(true)
    var hold by mutableStateOf(false)
    var idFrom by mutableStateOf<Long?>(null)
    var idTo by mutableStateOf<Long?>(null)
    private val rows = mutableStateListOf<SniffRow>()
    val picked = mutableStateListOf<Long>()

    fun checked(id: Long): Boolean {
        if (!modeAll && idFrom != null && idTo != null && id >= idFrom!! && id <= idTo!! && !picked.contains(id)) {
            return true
        }
        val listed = picked.contains(id)
        return if (modeAll) !listed else listed
    }

    fun json(): String {
        val mode = if (modeAll) "all" else "only"
        val body = picked.take(FilterMax).joinToString(",")
        val range = if (!modeAll && idFrom != null && idTo != null) {
            ""","id_from":$idFrom,"id_to":$idTo"""
        } else {
            ""
        }
        return """{"op":"set_filter","active":true,"mode":"$mode","ids":[$body]$range}"""
    }

    private fun bump() {
        rev++
        generation++
    }

    fun all() {
        modeAll = true
        picked.clear()
        idFrom = null
        idTo = null
        bump()
    }

    fun none() {
        modeAll = false
        picked.clear()
        idFrom = null
        idTo = null
        bump()
    }

    fun toggle(id: Long) {
        val on = checked(id)
        if (on && !modeAll && idFrom != null && idTo != null && id >= idFrom!! && id <= idTo!! && !picked.contains(id)) {
            idFrom = null
            idTo = null
            bump()
            return
        }
        if (modeAll) {
            if (on) {
                if (!picked.contains(id) && picked.size < FilterMax) picked.add(id)
            } else {
                picked.remove(id)
            }
        } else if (on) {
            picked.remove(id)
        } else if (!picked.contains(id) && picked.size < FilterMax) {
            picked.add(id)
        }
        bump()
    }

    fun add(raw: String, rawTo: String) {
        val id = raw.trim().toLongOrNull(16) ?: return
        val to = rawTo.trim().toLongOrNull(16)
        if (!modeAll && to != null) {
            idFrom = minOf(id, to)
            idTo = maxOf(id, to)
        } else if (!modeAll) {
            if (!picked.contains(id) && picked.size < FilterMax) picked.add(id)
        } else {
            picked.remove(id)
        }
        bump()
    }

    fun notch() {
        val now = System.currentTimeMillis()
        for (row in rows) {
            for (i in 0 until 8) {
                if (now - row.changedAt[i] < 200) {
                    row.notch[i] = row.notch[i] or (row.bytes[i] xor row.prev[i])
                }
            }
        }
        generation++
    }

    fun clearNotch() {
        for (row in rows) {
            for (i in 0 until 8) row.notch[i] = 0
        }
        generation++
    }

    fun ingest(batch: List<CanFrame>) {
        val now = System.currentTimeMillis()
        for (frame in batch) {
            val ext = (frame.flags and 1) != 0
            val incoming = IntArray(8) { i -> if (i < frame.data.size) frame.data[i].toInt() and 0xFF else 0 }
            val idx = rows.indexOfFirst { it.id == frame.id && it.ext == ext }
            if (idx < 0) {
                if (rows.size >= HeardMax) {
                    var quiet = 0
                    for (i in 1 until rows.size) {
                        if (rows[i].lastMs < rows[quiet].lastMs) quiet = i
                    }
                    rows.removeAt(quiet)
                }
                rows.add(SniffRow(frame.id, ext, incoming, incoming.copyOf(), LongArray(8), IntArray(8), now))
            } else {
                val row = rows[idx]
                for (i in 0 until 8) {
                    if (incoming[i] != row.bytes[i]) {
                        row.prev[i] = row.bytes[i]
                        row.bytes[i] = incoming[i]
                        row.changedAt[i] = now
                    }
                }
                row.lastMs = now
            }
        }
        generation++
    }

    private fun shown(now: Long): List<SniffRow> {
        return rows.filter { checked(it.id) && (hold || now - it.lastMs <= 5000) }.sortedBy { it.id }
    }

    fun knownIds(): List<Long> = (rows.map { it.id } + picked).distinct().sorted()

    @Composable
    fun Live(now: Long) {
        shown(now).forEach { row ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
                    .padding(vertical = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Checkbox(
                    checked = checked(row.id),
                    onCheckedChange = { toggle(row.id) },
                    colors = CheckboxDefaults.colors(
                        checkedColor = Color(0xFF31576D),
                        uncheckedColor = Color.White,
                        checkmarkColor = Color.White,
                    ),
                )
                Text(
                    "%X".format(row.id),
                    color = if (now - row.lastMs > 2000) Dim else Color.White,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.width(72.dp),
                )
                for (i in 0 until 8) {
                    Text(
                        "%02X".format(row.bytes[i]),
                        color = byteColor(row, i, now),
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier
                            .padding(horizontal = 2.dp)
                            .background(ByteBg)
                            .padding(horizontal = 2.dp, vertical = 2.dp),
                    )
                }
            }
        }
    }
}

private fun byteColor(row: SniffRow, index: Int, now: Long): Color {
    val mask = row.notch[index].inv() and 0xFF
    val visNew = row.bytes[index] and mask
    val visOld = row.prev[index] and mask
    if (now - row.changedAt[index] < 200 && visNew != visOld) {
        return if (visNew > visOld) Up else Down
    }
    if (now - row.lastMs > 2000) return Dim
    return Color.White
}

@Composable
fun SniffView(model: SniffModel) {
    var tick by mutableIntStateOf(0)
    var funnel by mutableStateOf(false)
    var draft by mutableStateOf("")
    var draftTo by mutableStateOf("")
    LaunchedEffect(Unit) {
        while (true) {
            delay(200)
            tick++
        }
    }
    val now = System.currentTimeMillis() + if (tick < 0 || model.generation < 0) 0 else 0
    Column(Modifier.fillMaxWidth()) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Mark("▾", "Filter") { funnel = true }
            Text(
                "⌐",
                color = Color.White,
                fontSize = 22.sp,
                modifier = Modifier.pointerInput(model) {
                    detectTapGestures(
                        onTap = { model.notch() },
                        onLongPress = { model.clearNotch() },
                    )
                },
            )
            Mark(if (model.hold) "●" else "○", "Hold") { model.hold = !model.hold }
        }
        model.Live(now)
    }
    if (funnel) {
        val known = model.knownIds()
        AlertDialog(
            onDismissRequest = { funnel = false },
            containerColor = Color(CangoGrey),
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        TextButton(onClick = { model.all() }, colors = ButtonDefaults.textButtonColors(contentColor = Color.White)) {
                            Text("All")
                        }
                        TextButton(onClick = { model.none() }, colors = ButtonDefaults.textButtonColors(contentColor = Color.White)) {
                            Text("None")
                        }
                    }
                    OutlinedTextField(draft, { draft = it }, colors = fieldColors())
                    OutlinedTextField(draftTo, { draftTo = it }, colors = fieldColors())
                    TextButton(
                        onClick = {
                            model.add(draft, draftTo)
                            draft = ""
                            draftTo = ""
                        },
                        colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                    ) { Text("+") }
                    known.forEach { id ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Checkbox(
                                checked = model.checked(id),
                                onCheckedChange = { model.toggle(id) },
                                colors = CheckboxDefaults.colors(
                                    checkedColor = Color(0xFF31576D),
                                    uncheckedColor = Color.White,
                                    checkmarkColor = Color.White,
                                ),
                            )
                            Text("%X".format(id), color = Color.White, fontFamily = FontFamily.Monospace)
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { funnel = false }, colors = ButtonDefaults.textButtonColors(contentColor = Color.White)) {
                    Text("Close")
                }
            },
        )
    }
}

@Composable
private fun Mark(glyph: String, name: String, onClick: () -> Unit) {
    Text(
        glyph,
        color = Color.White,
        fontSize = 22.sp,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(8.dp)
            .semantics { contentDescription = name },
    )
}
