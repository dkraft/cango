package app.cango.ui

import android.util.Base64
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import org.json.JSONObject

private class CardRow(val name: String, val dir: Boolean, val bytes: Long)

/** One directory on the card. Notch and filters do not apply here. */
internal class CardModel {
    var path by mutableStateOf("")
    var skip by mutableIntStateOf(0)
    var generation by mutableIntStateOf(0)
    var more by mutableStateOf(false)
    var pending by mutableStateOf<String?>(null)
    var stamp by mutableIntStateOf(0)
    var seen by mutableStateOf(false)
    var view by mutableStateOf<String?>(null)
    var viewText by mutableStateOf("")
    var viewOffset by mutableIntStateOf(0)
    var viewMore by mutableStateOf(false)
    private val rows: SnapshotStateList<CardRow> = emptyList<CardRow>().toMutableStateList()

    fun request(): String = """{"op":"list_dir","path":${JSONObject.quote(path)},"skip":$skip}"""

    fun viewRequest(): String =
        """{"op":"read_file","path":${JSONObject.quote(view ?: "")},"offset":$viewOffset}"""

    fun deleteRequest(): String? {
        val rel = pending ?: return null
        return """{"op":"delete_path","path":${JSONObject.quote(rel)}}"""
    }

    fun refresh() {
        skip = 0
        seen = false
        generation++
    }

    fun reset() {
        path = ""
        skip = 0
        seen = false
        rows.clear()
        more = false
        pending = null
        closeView()
        generation++
    }

    fun up() {
        val cut = path.lastIndexOf('/')
        path = if (cut < 0) "" else path.substring(0, cut)
        skip = 0
        seen = false
        rows.clear()
        more = false
        pending = null
    }

    fun open(name: String) {
        path = if (path.isEmpty()) name else "$path/$name"
        skip = 0
        seen = false
        rows.clear()
        more = false
        pending = null
        closeView()
    }

    fun openView(rel: String) {
        view = rel
        viewText = ""
        viewOffset = 0
        viewMore = false
    }

    fun closeView() {
        view = null
        viewText = ""
        viewOffset = 0
        viewMore = false
    }

    fun page() {
        skip = rows.size
        seen = false
    }

    fun child(name: String): String = if (path.isEmpty()) name else "$path/$name"

    fun apply(obj: JSONObject) {
        if (obj.optString("path") != path || obj.optInt("skip") != skip) {
            return
        }
        if (!obj.optBoolean("ok")) {
            if (skip == 0) {
                rows.clear()
            }
            more = false
            stamp++
            return
        }
        if (skip == 0) {
            rows.clear()
        }
        val entries = obj.optJSONArray("entries")
        if (entries == null) {
            more = false
            stamp++
            return
        }
        for (i in 0 until entries.length()) {
            val entry = entries.getJSONObject(i)
            rows.add(CardRow(entry.optString("name"), entry.optBoolean("dir"), entry.optLong("bytes")))
        }
        more = obj.optBoolean("more")
        seen = true
        stamp++
    }

    fun applyRead(obj: JSONObject) {
        val rel = view ?: return
        if (obj.optString("path") != rel && obj.optString("path").isNotEmpty()) {
            return
        }
        if (!obj.optBoolean("ok")) {
            viewText = obj.optString("error")
            viewMore = false
            stamp++
            return
        }
        if (obj.optInt("offset") != viewOffset) {
            return
        }
        val raw = try {
            Base64.decode(obj.optString("data"), Base64.DEFAULT)
        } catch (_err: IllegalArgumentException) {
            ByteArray(0)
        }
        val piece = present(raw)
        viewText = if (viewOffset == 0) piece else viewText + piece
        viewOffset += obj.optInt("n")
        viewMore = obj.optBoolean("more") && viewText.length < 48000
        stamp++
    }

    @Composable
    fun Body(onOpenFile: (String) -> Unit) {
        Column(Modifier.fillMaxWidth()) {
            rows.forEach { row ->
                Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        if (row.dir) row.name + "/" else row.name,
                        color = Color.White,
                        modifier = Modifier
                            .weight(1f)
                            .clickable {
                                if (row.dir) open(row.name) else onOpenFile(child(row.name))
                            },
                    )
                    if (!row.dir) {
                        Text(
                            row.bytes.toString(),
                            color = Color(0xFF8E98A0),
                            modifier = Modifier.padding(end = 8.dp),
                        )
                    }
                    Text(
                        "×",
                        color = Color.White,
                        modifier = Modifier
                            .clickable { pending = child(row.name) }
                            .padding(8.dp)
                            .semantics { contentDescription = "Delete" },
                    )
                }
            }
        }
    }
}

private fun present(bytes: ByteArray): String {
    if (bytes.isEmpty()) {
        return ""
    }
    val text = bytes.all { b ->
        val c = b.toInt() and 0xff
        c == 9 || c == 10 || c == 13 || c in 32..126
    }
    if (text) {
        return bytes.toString(Charsets.UTF_8)
    }
    val out = StringBuilder()
    for (i in bytes.indices) {
        out.append("%02X".format(bytes[i].toInt() and 0xff))
        out.append(if (i % 16 == 15) '\n' else ' ')
    }
    return out.toString()
}

@Composable
internal fun FilesView(model: CardModel, onSend: (String) -> Unit) {
    model.stamp
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            WhiteButton(enabled = model.path.isNotEmpty(), onClick = { model.up() }) {
                Text("↑", color = Color.White, modifier = Modifier.semantics { contentDescription = "Up" })
            }
            Text(
                model.path,
                color = Color(0xFF8E98A0),
                modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            )
            WhiteButton(onClick = { model.refresh() }) {
                Text("↻", color = Color.White, modifier = Modifier.semantics { contentDescription = "Refresh" })
            }
        }
        val open = model.view
        if (open != null) {
            Text(open, color = Color(0xFF8E98A0), modifier = Modifier.padding(vertical = 8.dp))
            Text(
                model.viewText,
                color = Color.White,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.fillMaxWidth(),
            )
            Row {
                if (model.viewMore) {
                    WhiteButton(onClick = { onSend(model.viewRequest()) }) { Text("More", color = Color.White) }
                }
                WhiteButton(onClick = { model.closeView() }) { Text("Close", color = Color.White) }
            }
        }
        model.Body { rel ->
            model.openView(rel)
            onSend(model.viewRequest())
        }
        if (model.more) {
            WhiteButton(onClick = { model.page() }) {
                Text("↓", color = Color.White, modifier = Modifier.semantics { contentDescription = "More" })
            }
        }
    }
    val pending = model.pending
    if (pending != null) {
        AlertDialog(
            onDismissRequest = { model.pending = null },
            containerColor = Color(CangoGrey),
            title = { Text(pending, color = Color.White) },
            confirmButton = {
                TextButton(
                    onClick = {
                        val body = model.deleteRequest()
                        model.pending = null
                        if (body != null) {
                            onSend(body)
                        }
                    },
                    colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                ) { Text("Delete") }
            },
            dismissButton = {
                TextButton(
                    onClick = { model.pending = null },
                    colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
                ) { Text("Close") }
            },
        )
    }
}
