package app.cango.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.key
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.cango.CanFrame
import kotlinx.coroutines.delay

private const val KeepPackets = 80
private const val ShowPackets = 40
private const val ReplyMs = 80L
private const val ReplyHits = 2
private val Dim = Color(0xFF8E98A0)
private val Bad = Color(0xFFE25B5B)
private val Tags = listOf(
    0xFF3D7BFF.toInt() to "Blue",
    0xFF3DDC97.toInt() to "Green",
    0xFFE2B84A.toInt() to "Amber",
    0xFFE25B5B.toInt() to "Red",
    0xFFC07DFF.toInt() to "Violet",
)

internal data class IdMark(val name: String, val argb: Int)

internal class Packet(
    val tMs: Long,
    val id: Long,
    val ext: Boolean,
    val len: Int,
    val bytes: IntArray,
)

/** Live frames. The field is a display filter. An id list is also sent on BLE. */
class SniffModel {
    var rev by mutableIntStateOf(0)
    var generation by mutableIntStateOf(0)
    var demo by mutableStateOf(false)
    var filterText by mutableStateOf("")
    var filterBad by mutableStateOf(false)
    internal var marks by mutableStateOf<Map<Long, IdMark>>(emptyMap())
    var naming by mutableStateOf(-1L)
    private var applied: Expr? = null
    private var board = passAll()
    private var t0 = 0L
    private var watchId = -1L
    private var trackText = ""
    private var replyIds = emptySet<Long>()
    private val packets = mutableStateListOf<Packet>()

    fun json(): String = board

    fun setFilter(text: String) {
        filterText = text
        if (watchId >= 0 && text != trackText) {
            watchId = -1L
            trackText = ""
            replyIds = emptySet()
        }
        if (text.isBlank()) {
            filterBad = false
            applied = null
            publish(passAll())
            generation++
            return
        }
        val expr = compile(text)
        if (expr == null) {
            filterBad = true
            generation++
            return
        }
        filterBad = false
        applied = expr
        val allow = idAllow(expr).only
        publish(if (allow != null && allow.size <= 32) onlyIds(allow) else passAll())
        generation++
    }

    fun ingest(batch: List<CanFrame>) {
        val now = System.currentTimeMillis()
        if (t0 == 0L) {
            t0 = now
        }
        for (frame in batch) {
            val n = frame.data.size.coerceAtMost(8)
            packets.add(
                Packet(
                    now,
                    frame.id,
                    (frame.flags and 1) != 0,
                    n,
                    IntArray(n) { frame.data[it].toInt() and 0xff },
                ),
            )
        }
        while (packets.size > KeepPackets) {
            packets.removeAt(0)
        }
        pullReplies()
        generation++
    }

    /** Hold a row. Follow that id and the frames that answer it. */
    fun hold(id: Long) {
        naming = id
        watchId = id
        val extra = linked(id)
        replyIds = extra.toSet()
        val text = filterFor(id, extra)
        trackText = text
        setFilter(text)
    }

    fun saveMark(id: Long, name: String, argb: Int) {
        val next = marks.toMutableMap()
        if (name.isBlank() && argb == 0) {
            next.remove(id)
        } else {
            next[id] = IdMark(name, argb)
        }
        if (next == marks) {
            return
        }
        marks = next
        generation++
    }

    fun closeNaming() {
        naming = -1L
    }

    internal fun label(id: Long): String {
        val name = marks[id]?.name?.trim().orEmpty()
        return if (name.isEmpty()) "%X".format(id) else name
    }

    internal fun tint(id: Long): Color {
        val own = marks[id]?.argb ?: 0
        if (own != 0) {
            return Color(own)
        }
        val watch = watchId
        if (watch >= 0 && id in replyIds) {
            val argb = marks[watch]?.argb ?: 0
            if (argb != 0) {
                return Color(argb)
            }
        }
        return Color.White
    }

    private fun pullReplies() {
        val id = watchId
        if (id < 0 || filterText != trackText) {
            return
        }
        val extra = linked(id)
        val text = filterFor(id, extra)
        if (text == trackText) {
            return
        }
        replyIds = extra.toSet()
        trackText = text
        setFilter(text)
    }

    private fun linked(id: Long): List<Long> {
        val forward = HashMap<Long, Int>()
        val back = HashMap<Long, Int>()
        var seen = 0
        for (i in packets.indices) {
            if (packets[i].id != id) {
                continue
            }
            seen++
            val at = packets[i].tMs
            for (j in i + 1 until packets.size) {
                val gap = packets[j].tMs - at
                if (gap <= 0) {
                    continue
                }
                if (gap > ReplyMs) {
                    break
                }
                if (packets[j].id != id) {
                    forward[packets[j].id] = (forward[packets[j].id] ?: 0) + 1
                }
                break
            }
            for (j in i - 1 downTo 0) {
                val gap = at - packets[j].tMs
                if (gap <= 0) {
                    continue
                }
                if (gap > ReplyMs) {
                    break
                }
                if (packets[j].id != id) {
                    back[packets[j].id] = (back[packets[j].id] ?: 0) + 1
                }
                break
            }
        }
        val out = ArrayList<Long>()
        for ((key, n) in forward) {
            if (n >= ReplyHits && n * 2 >= seen) {
                out.add(key)
            }
        }
        for ((key, n) in back) {
            if (n >= ReplyHits && n * 2 >= seen && key !in out) {
                out.add(key)
            }
        }
        return out
    }

    internal fun rel(packet: Packet): String {
        val ms = (packet.tMs - t0).coerceAtLeast(0)
        return "%d.%03d".format(ms / 1000, (ms % 1000).toInt())
    }

    internal fun shown(): List<Packet> {
        val pred = applied
        val seq = if (pred == null) packets else packets.filter { eval(pred, it) }
        return seq.takeLast(ShowPackets).asReversed()
    }

    private fun publish(next: String) {
        if (next == board) {
            return
        }
        board = next
        rev++
    }
}

private fun passAll(): String = """{"op":"set_filter","active":true,"mode":"all","ids":[]}"""

private fun onlyIds(ids: List<Long>): String {
    val body = ids.distinct().sorted().joinToString(",")
    return """{"op":"set_filter","active":true,"mode":"only","ids":[$body]}"""
}

private fun filterFor(id: Long, extra: List<Long>): String {
    return (listOf(id) + extra).distinct().sorted().joinToString(" || ") { "id == 0x%X".format(it) }
}

@Composable
fun SniffView(model: SniffModel) {
    LaunchedEffect(model.demo) {
        if (!model.demo) {
            return@LaunchedEffect
        }
        var n = 0
        while (true) {
            val tick = (n and 0xff).toByte()
            model.ingest(listOf(CanFrame(0, 0x100, 0, byteArrayOf(tick, 0x10, 0, 0, 0, 0, 0, 1))))
            delay(40)
            model.ingest(listOf(CanFrame(0, 0x123, 0, byteArrayOf(0x02, 0x10, ((n / 4) and 0xff).toByte()))))
            delay(100)
            model.ingest(listOf(CanFrame(0, 0x200, 0, byteArrayOf(if (n % 2 == 0) 0 else 0x7f, 0x01))))
            n++
            delay(110)
        }
    }
    model.generation
    Column(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = model.filterText,
                onValueChange = { model.setFilter(it) },
                singleLine = true,
                isError = model.filterBad,
                label = { Text("Filter", color = Color.White) },
                placeholder = { Text("id == 0x123", color = Dim) },
                colors = OutlinedTextFieldDefaults.colors(
                    focusedTextColor = Color.White,
                    unfocusedTextColor = Color.White,
                    focusedLabelColor = Color.White,
                    unfocusedLabelColor = Color.White,
                    cursorColor = Color.White,
                    focusedBorderColor = Color.White,
                    unfocusedBorderColor = Dim,
                    errorBorderColor = Bad,
                    errorLabelColor = Bad,
                    errorCursorColor = Bad,
                    errorTextColor = Color.White,
                ),
                modifier = Modifier.weight(1f).semantics { contentDescription = "Filter" },
            )
            WhiteButton(onClick = { model.demo = !model.demo }) {
                Text(
                    if (model.demo) "Stop" else "Demo",
                    color = Color.White,
                    modifier = Modifier.semantics { contentDescription = "Demo" },
                )
            }
        }
        val live = model.shown()
        val liveState = rememberUpdatedState(live)
        var pressed by remember { mutableStateOf(false) }
        var frozen by remember { mutableStateOf<List<Packet>>(emptyList()) }
        val rows = if (pressed) frozen else live
        Column(Modifier.horizontalScroll(rememberScrollState())) {
            Row(Modifier.padding(bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                Head("Time", 68)
                Head("ID", 40)
                Head("Len", 20)
                Head("Data", 0)
            }
            rows.forEach { packet ->
                key(packet) {
                val held = rememberUpdatedState(packet.id)
                Row(
                    Modifier
                        .padding(vertical = 2.dp)
                        .pointerInput(packet) {
                            detectTapGestures(
                                onPress = {
                                    frozen = liveState.value.toList()
                                    pressed = true
                                    try {
                                        tryAwaitRelease()
                                    } finally {
                                        pressed = false
                                    }
                                },
                                onLongPress = { model.hold(held.value) },
                            )
                        },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Cell(model.rel(packet), 68)
                    Cell(model.label(packet.id), 40, model.tint(packet.id))
                    Cell(packet.len.toString(), 20)
                    Text(
                        (0 until packet.len).joinToString(" ") { "%02X".format(packet.bytes[it]) },
                        color = Color.White,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 13.sp,
                        maxLines = 1,
                        softWrap = false,
                        overflow = TextOverflow.Visible,
                    )
                }
                }
            }
        }
        val naming = model.naming
        if (naming >= 0L) {
            NameDialog(model, naming)
        }
    }
}

@Composable
private fun NameDialog(model: SniffModel, id: Long) {
    val mark = model.marks[id]
    val name = mark?.name.orEmpty()
    val argb = mark?.argb ?: 0
    AlertDialog(
        onDismissRequest = { model.closeNaming() },
        containerColor = Color(CangoGrey),
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("%X".format(id), color = Color.White, fontFamily = FontFamily.Monospace)
                OutlinedTextField(
                    value = name,
                    onValueChange = { model.saveMark(id, it, argb) },
                    singleLine = true,
                    label = { Text("Name", color = Color.White) },
                    colors = fieldColors(),
                    modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Name" },
                )
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    for ((color, label) in Tags) {
                        val chosen = color == argb
                        Box(
                            Modifier
                                .size(32.dp)
                                .background(Color(color), CircleShape)
                                .border(2.dp, if (chosen) Color.White else Color(color), CircleShape)
                                .clickable {
                                    model.saveMark(id, name, if (chosen) 0 else color)
                                }
                                .semantics { contentDescription = label },
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { model.closeNaming() },
                colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
            ) { Text("Close") }
        },
    )
}

@Composable
private fun Head(text: String, min: Int) {
    Text(
        text,
        color = Dim,
        fontSize = 13.sp,
        maxLines = 1,
        softWrap = false,
        modifier = Modifier.widthIn(min = min.dp).padding(end = 8.dp),
    )
}

@Composable
private fun Cell(text: String, min: Int, color: Color = Color.White) {
    Text(
        text,
        color = color,
        fontFamily = FontFamily.Monospace,
        fontSize = 13.sp,
        maxLines = 1,
        softWrap = false,
        modifier = Modifier.widthIn(min = min.dp).padding(end = 8.dp),
    )
}

private sealed class Expr {
    data class Cmp(val field: Field, val op: String, val value: Long) : Expr()
    data class Flag(val ext: Boolean) : Expr()
    data class Contains(val bytes: IntArray) : Expr()
    data class Not(val inner: Expr) : Expr()
    data class And(val left: Expr, val right: Expr) : Expr()
    data class Or(val left: Expr, val right: Expr) : Expr()
}

private sealed class Field {
    data object Id : Field()
    data object Len : Field()
    data class Data(val index: Int) : Field()
}

private class IdAllow(val only: List<Long>?)

private fun idAllow(expr: Expr): IdAllow = when (expr) {
    is Expr.Cmp -> if (expr.field == Field.Id && expr.op == "==") IdAllow(listOf(expr.value)) else IdAllow(null)
    is Expr.Flag, is Expr.Contains, is Expr.Not -> IdAllow(null)
    is Expr.Or -> {
        val left = idAllow(expr.left).only
        val right = idAllow(expr.right).only
        if (left != null && right != null) IdAllow((left + right).distinct()) else IdAllow(null)
    }
    is Expr.And -> {
        val left = idAllow(expr.left).only
        val right = idAllow(expr.right).only
        when {
            left != null && right != null -> IdAllow(left.intersect(right.toSet()).toList())
            left != null && narrowable(expr.right) -> IdAllow(left)
            right != null && narrowable(expr.left) -> IdAllow(right)
            else -> IdAllow(null)
        }
    }
}

private fun narrowable(expr: Expr): Boolean = when (expr) {
    is Expr.Cmp -> expr.field != Field.Id || expr.op == "=="
    is Expr.Flag, is Expr.Contains -> true
    is Expr.Not -> narrowable(expr.inner)
    is Expr.And -> narrowable(expr.left) && narrowable(expr.right)
    is Expr.Or -> false
}

private fun eval(expr: Expr, packet: Packet): Boolean = when (expr) {
    is Expr.Or -> eval(expr.left, packet) || eval(expr.right, packet)
    is Expr.And -> eval(expr.left, packet) && eval(expr.right, packet)
    is Expr.Not -> !eval(expr.inner, packet)
    is Expr.Flag -> packet.ext == expr.ext
    is Expr.Contains -> contains(packet, expr.bytes)
    is Expr.Cmp -> compare(expr.op, fieldValue(expr.field, packet), expr.value)
}

private fun fieldValue(field: Field, packet: Packet): Long = when (field) {
    Field.Id -> packet.id
    Field.Len -> packet.len.toLong()
    is Field.Data -> if (field.index < packet.len) packet.bytes[field.index].toLong() else -1
}

private fun compare(op: String, left: Long, right: Long): Boolean = when (op) {
    "==" -> left == right
    "!=" -> left != right
    ">" -> left > right
    ">=" -> left >= right
    "<" -> left < right
    "<=" -> left <= right
    else -> false
}

private fun contains(packet: Packet, needle: IntArray): Boolean {
    if (needle.isEmpty() || needle.size > packet.len) {
        return needle.isEmpty()
    }
    for (start in 0..packet.len - needle.size) {
        var same = true
        for (i in needle.indices) {
            if (packet.bytes[start + i] != needle[i]) {
                same = false
            }
        }
        if (same) {
            return true
        }
    }
    return false
}

private class Tok(val kind: String, val text: String)

private fun lex(src: String): List<Tok>? {
    val out = ArrayList<Tok>()
    var i = 0
    while (i < src.length) {
        val c = src[i]
        if (c.isWhitespace()) {
            i++
            continue
        }
        if (c == '!' && i + 1 < src.length && src[i + 1] == '=') {
            out.add(Tok("op", "!="))
            i += 2
            continue
        }
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '!') {
            out.add(Tok(c.toString(), c.toString()))
            i++
            continue
        }
        if ((c == '&' || c == '|') && i + 1 < src.length && src[i + 1] == c) {
            out.add(Tok(if (c == '&') "and" else "or", ""))
            i += 2
            continue
        }
        if ((c == '=' || c == '>' || c == '<') && i + 1 < src.length && src[i + 1] == '=') {
            out.add(Tok("op", src.substring(i, i + 2)))
            i += 2
            continue
        }
        if (c == '>' || c == '<') {
            out.add(Tok("op", c.toString()))
            i++
            continue
        }
        if (c == '0' && i + 1 < src.length && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
            var j = i + 2
            while (j < src.length && src[j].isHex()) j++
            if (j == i + 2) {
                return null
            }
            out.add(Tok("num", src.substring(i, j)))
            i = j
            continue
        }
        if (c.isDigit()) {
            var j = i
            while (j < src.length && src[j].isDigit()) j++
            out.add(Tok("num", src.substring(i, j)))
            i = j
            continue
        }
        if (c.isLetter()) {
            var j = i
            while (j < src.length && (src[j].isLetterOrDigit() || src[j] == '.' || src[j] == '_')) j++
            out.add(Tok("id", src.substring(i, j)))
            i = j
            continue
        }
        return null
    }
    return out
}

private fun Char.isHex(): Boolean = this.isDigit() || this.lowercaseChar() in 'a'..'f'

private fun number(text: String): Long? {
    return if (text.startsWith("0x") || text.startsWith("0X")) {
        text.drop(2).toLongOrNull(16)
    } else {
        text.toLongOrNull(10)
    }
}

private fun hexByte(text: String): Int? {
    val n = if (text.startsWith("0x") || text.startsWith("0X")) {
        text.drop(2).toLongOrNull(16)
    } else {
        text.toLongOrNull(16)
    }
    if (n == null || n !in 0..255) {
        return null
    }
    return n.toInt()
}

private fun fieldKind(name: String): String = when (name.lowercase()) {
    "id", "can.id" -> "id"
    "len", "can.len", "dlc" -> "len"
    "ext", "can.flags.ext" -> "ext"
    "std" -> "std"
    "data", "can.data" -> "data"
    else -> ""
}

private class Parser(val tok: List<Tok>) {
    var i = 0

    fun peek(): Tok? = tok.getOrNull(i)

    fun eat(kind: String): Tok? {
        val item = peek()
        if (item == null || item.kind != kind) {
            return null
        }
        i++
        return item
    }

    fun parse(): Expr? {
        val expr = parseOr() ?: return null
        if (i != tok.size) {
            return null
        }
        return expr
    }

    private fun parseOr(): Expr? {
        var left = parseAnd() ?: return null
        while (eat("or") != null) {
            val right = parseAnd() ?: return null
            left = Expr.Or(left, right)
        }
        return left
    }

    private fun parseAnd(): Expr? {
        var left = parseUnary() ?: return null
        while (eat("and") != null) {
            val right = parseUnary() ?: return null
            left = Expr.And(left, right)
        }
        return left
    }

    private fun parseUnary(): Expr? {
        if (eat("!") != null) {
            val inner = parseUnary() ?: return null
            return Expr.Not(inner)
        }
        return parsePrimary()
    }

    private fun parsePrimary(): Expr? {
        if (eat("(") != null) {
            val inner = parseOr() ?: return null
            if (eat(")") == null) {
                return null
            }
            return inner
        }
        val name = eat("id") ?: return null
        when (fieldKind(name.text)) {
            "ext" -> return Expr.Flag(true)
            "std" -> return Expr.Flag(false)
            "data" -> return parseData()
            "id", "len" -> return parseCmp(if (fieldKind(name.text) == "id") Field.Id else Field.Len)
            else -> return null
        }
    }

    private fun parseData(): Expr? {
        if (eat("[") != null) {
            val indexTok = eat("num") ?: return null
            if (eat("]") == null) {
                return null
            }
            val index = number(indexTok.text) ?: return null
            if (index !in 0..7) {
                return null
            }
            val op = eat("op") ?: return null
            val valueTok = eat("num") ?: return null
            val value = number(valueTok.text) ?: return null
            if (value !in 0..255) {
                return null
            }
            return Expr.Cmp(Field.Data(index.toInt()), op.text, value)
        }
        val word = peek()
        if (word != null && word.kind == "id" && word.text.equals("contains", ignoreCase = true)) {
            i++
            val bytes = ArrayList<Int>()
            while (peek()?.kind == "num") {
                val item = eat("num") ?: return null
                val value = hexByte(item.text) ?: return null
                bytes.add(value)
            }
            if (bytes.isEmpty()) {
                return null
            }
            return Expr.Contains(bytes.toIntArray())
        }
        return null
    }

    private fun parseCmp(field: Field): Expr? {
        val op = eat("op") ?: return null
        val valueTok = eat("num") ?: return null
        val value = number(valueTok.text) ?: return null
        if (field == Field.Len && value !in 0..8) {
            return null
        }
        if (field == Field.Id && (value < 0 || value > 0x1FFFFFFF)) {
            return null
        }
        return Expr.Cmp(field, op.text, value)
    }
}

private fun compile(text: String): Expr? {
    val tok = lex(text) ?: return null
    if (tok.isEmpty()) {
        return null
    }
    return Parser(tok).parse()
}
