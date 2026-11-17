#include "app.h"
#include "board.h"
#include "storage.h"

#include "cango_project.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wifi_offer_frame(const cango_frame_t *frame);
void ble_offer_frame(const cango_frame_t *frame);

static cango_filter_state_t s_wifi_filt;
static cango_filter_state_t s_ble_filt;
static uint64_t s_unix_base;
static uint32_t s_unix_mark;
static char s_note[160];
static int s_note_link = -1;
static volatile bool s_note_ready;
static volatile bool s_formatting;

static void session_note(int link, const char *line)
{
    app_lock();
    snprintf(s_note, sizeof(s_note), "%s", line);
    s_note_link = link;
    s_note_ready = true;
    app_unlock();
}

int session_poll(int link, char *out, int cap)
{
    int n;
    if (!s_note_ready || out == NULL || cap < 8 || s_note_link != link) {
        return 0;
    }
    app_lock();
    if (!s_note_ready || s_note_link != link) {
        app_unlock();
        return 0;
    }
    n = (int)strlen(s_note);
    if (n + 1 > cap) {
        app_unlock();
        return 0;
    }
    memcpy(out, s_note, (size_t)n + 1);
    s_note_ready = false;
    app_unlock();
    return n;
}

static void format_task(void *arg)
{
    int link = (int)(intptr_t)arg;
    const char *err = storage_format_card();
    if (err == NULL) {
        session_note(link, "{\"op\":\"format_card\",\"ok\":true,\"state\":\"done\"}\n");
    } else {
        char line[96];
        snprintf(line, sizeof(line), "{\"op\":\"format_card\",\"ok\":false,\"error\":\"%s\",\"state\":\"done\"}\n", err);
        session_note(link, line);
    }
    s_formatting = false;
    vTaskDelete(NULL);
}

static const char *skip(const char *s)
{
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
        s++;
    }
    return s;
}

static const char *key_at(const char *json, const char *key)
{
    char pat[40];
    const char *p = json;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    while ((p = strstr(p, pat)) != NULL) {
        const char *after = skip(p + strlen(pat));
        if (*after == ':') {
            return skip(after + 1);
        }
        p += strlen(pat);
    }
    return NULL;
}

static bool grab_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = key_at(json, key);
    size_t i = 0;
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    /* Android's JSONObject.quote writes a slash as \/. A nested card path
     * arrives that way, and the card rejects a backslash. */
    while (*p != '\0' && i + 1 < cap) {
        if (*p == '"') {
            break;
        }
        if (*p == '\\') {
            p++;
            if (*p == '\0') {
                break;
            }
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return *p == '"';
}

static bool grab_u32(const char *json, const char *key, uint32_t *out)
{
    const char *p = key_at(json, key);
    char *end = NULL;
    unsigned long v;
    if (p == NULL) {
        return false;
    }
    v = strtoul(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool grab_bool(const char *json, const char *key, bool *out)
{
    const char *p = key_at(json, key);
    if (p == NULL) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static const char *mode_name(uint8_t mode)
{
    if (mode == CANGO_MODE_BLE) {
        return "ble";
    }
    if (mode == CANGO_MODE_WIFI) {
        return "wifi";
    }
    return "capture";
}

bool ctrl_capture_running(void);
bool ctrl_capture_complete(void);

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int reply(char *out, int cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(out, (size_t)cap, fmt, ap);
    va_end(ap);
    return n;
}

void session_status(char *out, int cap)
{
    const char *cap_state = "idle";
    app_lock();
    if (ctrl_capture_running()) {
        cap_state = "running";
    } else if (g_app.mode == CANGO_MODE_CAPTURE && ctrl_capture_complete()) {
        cap_state = "complete";
    }
    snprintf(out, (size_t)cap,
             "{\"op\":\"bus_status\",\"state\":\"%s\",\"tec\":%" PRIu32 ",\"rec\":%" PRIu32
             ",\"rx\":%" PRIu32 ",\"missed\":%" PRIu32
             ",\"dropped\":%" PRIu32 ",\"sd\":%s,\"capture\":\"%s\",\"mode\":\"%s\",\"bitrate\":%" PRIu32
             ",\"listen_only\":%s,\"armed\":%s,\"profile\":\"%s\",\"project\":\"%s\","
             "\"ap\":\"%s\",\"sta\":%s}\n",
             g_app.bus_state, g_app.tec, g_app.rec, g_app.rx_count, g_app.missed, g_app.dropped,
             g_app.sd_ok ? "true" : "false", cap_state, mode_name(g_app.mode), g_app.bitrate,
             g_app.listen_only ? "true" : "false", g_app.armed ? "true" : "false",
             "30s",
             g_app.project.id[0] ? g_app.project.id : "generic",
             g_app.ap_ssid, g_app.sta_up ? "true" : "false");
    app_unlock();
}

static cango_filter_t *link_filter(int link)
{
    return link == LINK_BLE ? &g_app.ble_filter : &g_app.wifi_filter;
}

int session_handle(const char *json, char *out, int cap, int link)
{
    char op[32];
    if (json == NULL || out == NULL || cap < 8) {
        return -1;
    }
    if (!grab_str(json, "op", op, sizeof(op))) {
        return reply(out, cap, "{\"ok\":false,\"error\":\"no_op\"}\n");
    }
    if (strcmp(op, "hello") == 0) {
        return reply(out, cap, "{\"op\":\"hello\",\"ok\":true,\"version\":\"%s\",\"mode\":\"%s\",\"ap\":\"%s\"}\n",
                     CANGO_VERSION, mode_name(g_app.mode), g_app.ap_ssid);
    }
    if (strcmp(op, "get_projects") == 0) {
        return storage_projects_json(out, cap);
    }
    if (strcmp(op, "list_logs") == 0) {
        return storage_logs_json(out, cap);
    }
    if (strcmp(op, "list_dir") == 0) {
        char rel[96];
        uint32_t skip = 0;
        int n;
        rel[0] = '\0';
        if (key_at(json, "path") != NULL && !grab_str(json, "path", rel, sizeof(rel))) {
            return reply(out, cap, "{\"op\":\"list_dir\",\"ok\":false,\"error\":\"bad\",\"path\":\"\"}\n");
        }
        grab_u32(json, "skip", &skip);
        n = storage_dir_json(out, cap, rel, (int)skip);
        if (n < 0) {
            return reply(out, cap, "{\"op\":\"list_dir\",\"ok\":false,\"error\":\"full\",\"path\":\"\"}\n");
        }
        return n;
    }
    if (strcmp(op, "delete_path") == 0) {
        char rel[96];
        const char *err;
        if (!grab_str(json, "path", rel, sizeof(rel)) || rel[0] == '\0') {
            return reply(out, cap, "{\"op\":\"delete_path\",\"ok\":false,\"error\":\"bad\"}\n");
        }
        err = storage_delete_path(rel);
        if (err != NULL) {
            return reply(out, cap, "{\"op\":\"delete_path\",\"ok\":false,\"error\":\"%s\"}\n", err);
        }
        return reply(out, cap, "{\"op\":\"delete_path\",\"ok\":true}\n");
    }
    if (strcmp(op, "read_file") == 0) {
        char rel[96];
        uint32_t offset = 0;
        int n;
        if (!grab_str(json, "path", rel, sizeof(rel)) || rel[0] == '\0') {
            return reply(out, cap, "{\"op\":\"read_file\",\"ok\":false,\"error\":\"bad\",\"path\":\"\"}\n");
        }
        grab_u32(json, "offset", &offset);
        n = storage_read_file(out, cap, rel, offset);
        if (n <= 0 || n >= cap) {
            return reply(out, cap, "{\"op\":\"read_file\",\"ok\":false,\"error\":\"full\",\"path\":\"\"}\n");
        }
        return n;
    }
    if (strcmp(op, "format_card") == 0) {
        bool confirm = false;
        char open_dir[96];
        grab_bool(json, "confirm", &confirm);
        if (!confirm) {
            return reply(out, cap, "{\"op\":\"format_card\",\"ok\":false,\"error\":\"confirm\"}\n");
        }
        ctrl_capture_dir(open_dir, (int)sizeof(open_dir));
        if (open_dir[0] != '\0' || s_formatting) {
            return reply(out, cap, "{\"op\":\"format_card\",\"ok\":false,\"error\":\"busy\"}\n");
        }
        if (!g_app.sd_ok) {
            return reply(out, cap, "{\"op\":\"format_card\",\"ok\":false,\"error\":\"sd\"}\n");
        }
        s_formatting = true;
        if (xTaskCreate(format_task, "fmt", 12288, (void *)(intptr_t)link, 4, NULL) != pdPASS) {
            s_formatting = false;
            return reply(out, cap, "{\"op\":\"format_card\",\"ok\":false,\"error\":\"fail\"}\n");
        }
        return reply(out, cap, "{\"op\":\"format_card\",\"ok\":true,\"state\":\"started\"}\n");
    }
    if (strcmp(op, "capture_status") == 0 || strcmp(op, "status") == 0) {
        session_status(out, cap);
        return (int)strlen(out);
    }
    if (strcmp(op, "discover") == 0 || strcmp(op, "run_action") == 0) {
        return reply(out, cap, "{\"op\":\"%s\",\"ok\":false,\"error\":\"not_implemented\"}\n", op);
    }
    if (strcmp(op, "set_filter") == 0) {
        cango_filter_t *f = link_filter(link);
        bool active = true;
        bool have_ids = false;
        char ext[8];
        char mode[8];
        uint32_t ids[CANGO_FILTER_IDS];
        uint32_t n = 0;
        int id_n = 0;
        const char *id_list = key_at(json, "ids");
        if (id_list != NULL) {
            const char *p = id_list;
            have_ids = true;
            if (*p != '[') {
                return reply(out, cap, "{\"op\":\"set_filter\",\"ok\":false,\"error\":\"bad\"}\n");
            }
            p++;
            while (*p != '\0' && *p != ']') {
                char *end = NULL;
                unsigned long v;
                p = skip(p);
                if (*p == ']' || *p == '\0') {
                    break;
                }
                v = strtoul(p, &end, 10);
                if (end == p) {
                    return reply(out, cap, "{\"op\":\"set_filter\",\"ok\":false,\"error\":\"bad\"}\n");
                }
                if (id_n >= CANGO_FILTER_IDS) {
                    return reply(out, cap, "{\"op\":\"set_filter\",\"ok\":false,\"error\":\"full\"}\n");
                }
                ids[id_n++] = (uint32_t)v;
                p = skip(end);
                if (*p == ',') {
                    p++;
                }
            }
        }
        grab_bool(json, "active", &active);
        app_lock();
        cango_filter_clear(f);
        if (active && !have_ids) {
            f->active = true;
            f->mode = CANGO_FILTER_MODE_MASK;
            if (grab_u32(json, "id", &n)) {
                f->id = n;
            }
            if (grab_u32(json, "mask", &n)) {
                f->mask = n;
            } else {
                f->mask = 0x7ff;
            }
        } else if (active) {
            uint32_t id_from = 0;
            uint32_t id_to = 0;
            bool has_from = grab_u32(json, "id_from", &id_from);
            bool has_to = grab_u32(json, "id_to", &id_to);
            f->active = true;
            f->id_n = (uint8_t)id_n;
            if (id_n > 0) {
                memcpy(f->ids, ids, (size_t)id_n * sizeof(ids[0]));
            }
            if (grab_str(json, "mode", mode, sizeof(mode)) && strcmp(mode, "only") == 0) {
                f->mode = CANGO_FILTER_MODE_ONLY;
            } else {
                f->mode = CANGO_FILTER_MODE_ALL;
            }
            if (has_from && has_to) {
                f->has_range = true;
                f->id_from = id_from;
                f->id_to = id_to;
            }
        }
        if (f->active) {
            if (grab_str(json, "extended", ext, sizeof(ext))) {
                if (strcmp(ext, "std") == 0) {
                    f->extended = CANGO_EXT_STD;
                } else if (strcmp(ext, "ext") == 0) {
                    f->extended = CANGO_EXT_EXT;
                }
            }
            grab_bool(json, "change_only", &f->change_only);
            if (grab_u32(json, "min_interval_ms", &n) && n <= 65535) {
                f->min_interval_ms = (uint16_t)n;
            }
        }
        memset(link == LINK_BLE ? &s_ble_filt : &s_wifi_filt, 0, sizeof(cango_filter_state_t));
        app_unlock();
        if (link == LINK_BLE) {
            ble_heard_reset();
        }
        return reply(out, cap, "{\"op\":\"set_filter\",\"ok\":true}\n");
    }
    if (strcmp(op, "set_capture") == 0) {
        return reply(out, cap, "{\"op\":\"set_capture\",\"ok\":true}\n");
    }
    if (strcmp(op, "put_project") == 0) {
        cango_project_t p;
        if (!cango_project_parse(json, &p) || p.id[0] == 0 || cango_project_is_generic(p.id)) {
            return reply(out, cap, "{\"op\":\"put_project\",\"ok\":false,\"error\":\"rejected\"}\n");
        }
        if (!storage_save_project(&p)) {
            return reply(out, cap, "{\"op\":\"put_project\",\"ok\":false,\"error\":\"sd\"}\n");
        }
        return reply(out, cap, "{\"op\":\"put_project\",\"ok\":true}\n");
    }
    if (strcmp(op, "delete_project") == 0) {
        char id[CANGO_PROJECT_ID_LEN];
        if (!grab_str(json, "id", id, sizeof(id)) || !storage_delete_project(id)) {
            return reply(out, cap, "{\"op\":\"delete_project\",\"ok\":false}\n");
        }
        return reply(out, cap, "{\"op\":\"delete_project\",\"ok\":true}\n");
    }
    if (strcmp(op, "apply_project") == 0) {
        char id[CANGO_PROJECT_ID_LEN];
        cango_project_t p;
        char path[96];
        FILE *f;
        char body[1024];
        size_t nread;
        bool from_file = false;
        cango_project_generic(&p);
        if (!grab_str(json, "id", id, sizeof(id))) {
            return reply(out, cap, "{\"op\":\"apply_project\",\"ok\":false}\n");
        }
        if (!cango_project_is_generic(id) && g_app.sd_ok) {
            snprintf(path, sizeof(path), "/sdcard/projects/%s.json", id);
            f = fopen(path, "r");
            if (f != NULL) {
                nread = fread(body, 1, sizeof(body) - 1, f);
                fclose(f);
                body[nread] = 0;
                if (!cango_project_parse(body, &p)) {
                    return reply(out, cap, "{\"op\":\"apply_project\",\"ok\":false,\"error\":\"bad_json\"}\n");
                }
                from_file = true;
            }
        }
        if (!from_file && !cango_project_builtin(id, &p)) {
            return reply(out, cap, "{\"op\":\"apply_project\",\"ok\":false,\"error\":\"missing\"}\n");
        }
        app_lock();
        g_app.project = p;
        g_app.logging = p.logging;
        g_app.bitrate = p.bitrate;
        app_unlock();
        storage_save_logging();
        if (g_app.mode != CANGO_MODE_CAPTURE) {
            can_request_listen(p.bitrate);
        }
        return reply(out, cap, "{\"op\":\"apply_project\",\"ok\":true}\n");
    }
    if (strcmp(op, "arm_tx") == 0) {
        bool confirm = false;
        grab_bool(json, "confirm", &confirm);
        if (!confirm) {
            return reply(out, cap, "{\"op\":\"arm_tx\",\"ok\":false,\"error\":\"confirm\"}\n");
        }
        app_lock();
        g_app.armed = true;
        g_app.arm_until_us = app_now_us() + CANGO_ARM_US;
        app_unlock();
        can_request_normal(g_app.bitrate);
        return reply(out, cap, "{\"op\":\"arm_tx\",\"ok\":true}\n");
    }
    if (strcmp(op, "disarm") == 0) {
        app_lock();
        g_app.armed = false;
        app_unlock();
        can_request_listen(g_app.bitrate);
        return reply(out, cap, "{\"op\":\"disarm\",\"ok\":true}\n");
    }
    if (strcmp(op, "send_frame") == 0) {
        cango_frame_t frame;
        uint32_t id = 0;
        char hex[20];
        const char *data;
        memset(&frame, 0, sizeof(frame));
        if (!g_app.armed || app_now_us() > g_app.arm_until_us) {
            return reply(out, cap, "{\"op\":\"send_frame\",\"ok\":false,\"error\":\"disarmed\"}\n");
        }
        if (!grab_u32(json, "id", &id) || !cango_tx_allowed(&g_app.project, id)) {
            return reply(out, cap, "{\"op\":\"send_frame\",\"ok\":false,\"error\":\"not_allowed\"}\n");
        }
        frame.id = id;
        frame.flags = CANGO_F_TX;
        frame.t_us = app_now_us();
        if (grab_str(json, "data", hex, sizeof(hex))) {
            size_t i = 0;
            size_t len = strlen(hex);
            while (i + 1 < len && frame.len < 8) {
                int hi = hex_nibble(hex[i]);
                int lo = hex_nibble(hex[i + 1]);
                if (hi < 0 || lo < 0) {
                    break;
                }
                frame.data[frame.len++] = (uint8_t)((hi << 4) | lo);
                i += 2;
            }
        } else if ((data = key_at(json, "data")) != NULL && *data == '[') {
            data++;
            while (*data && *data != ']' && frame.len < 8) {
                char *end = NULL;
                unsigned long v = strtoul(data, &end, 10);
                if (end == data) {
                    break;
                }
                frame.data[frame.len++] = (uint8_t)v;
                data = end;
                if (*data == ',') {
                    data++;
                }
            }
        }
        if (!can_request_send(&frame)) {
            return reply(out, cap, "{\"op\":\"send_frame\",\"ok\":false,\"error\":\"queue\"}\n");
        }
        return reply(out, cap, "{\"op\":\"send_frame\",\"ok\":true}\n");
    }
    if (strcmp(op, "set_time") == 0) {
        uint32_t unix_s = 0;
        if (!grab_u32(json, "unix_s", &unix_s)) {
            return reply(out, cap, "{\"op\":\"set_time\",\"ok\":false}\n");
        }
        s_unix_base = unix_s;
        s_unix_mark = app_now_us();
        return reply(out, cap, "{\"op\":\"set_time\",\"ok\":true}\n");
    }
    if (strcmp(op, "set_wifi") == 0) {
        uint32_t slot = 0;
        char ssid[CANGO_SSID_LEN];
        char pass[CANGO_PASS_LEN];
        if (!grab_str(json, "ssid", ssid, sizeof(ssid)) || !grab_str(json, "password", pass, sizeof(pass))) {
            return reply(out, cap, "{\"op\":\"set_wifi\",\"ok\":false}\n");
        }
        grab_u32(json, "slot", &slot);
        if (slot >= CANGO_STA_MAX) {
            return reply(out, cap, "{\"op\":\"set_wifi\",\"ok\":false,\"error\":\"slot\"}\n");
        }
        app_lock();
        memset(&g_app.sta[slot], 0, sizeof(g_app.sta[slot]));
        memcpy(g_app.sta[slot].ssid, ssid, sizeof(g_app.sta[slot].ssid));
        memcpy(g_app.sta[slot].pass, pass, sizeof(g_app.sta[slot].pass));
        if ((int)slot >= g_app.sta_n) {
            g_app.sta_n = (int)slot + 1;
        }
        app_unlock();
        storage_save_sta();
        return reply(out, cap, "{\"op\":\"set_wifi\",\"ok\":true}\n");
    }
    if (strcmp(op, "get_wifi") == 0) {
        int n = 0;
        int i;
        app_lock();
        n = snprintf(out, (size_t)cap, "{\"op\":\"get_wifi\",\"ok\":true,\"entries\":[");
        for (i = 0; i < g_app.sta_n && i < CANGO_STA_MAX; i++) {
            int w = snprintf(out + n, (size_t)(cap - n), "%s{\"slot\":%d,\"ssid\":\"%s\",\"password\":\"%s\"}",
                             i ? "," : "", i, g_app.sta[i].ssid, g_app.sta[i].pass);
            if (w < 0 || n + w >= cap) {
                app_unlock();
                return -1;
            }
            n += w;
        }
        app_unlock();
        if (n + 4 >= cap) {
            return -1;
        }
        memcpy(out + n, "]}\n", 3);
        return n + 2;
    }
    return reply(out, cap, "{\"op\":\"%s\",\"ok\":false,\"error\":\"unknown\"}\n", op);
}

uint64_t session_unix_s(void)
{
    uint32_t now;
    if (s_unix_base == 0) {
        return 0;
    }
    now = app_now_us();
    return s_unix_base + (uint32_t)(now - s_unix_mark) / 1000000u;
}

void session_offer_frame(const cango_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    if (g_app.mode == CANGO_MODE_WIFI) {
        gvret_offer_frame(frame);
        if (cango_filter_match(&g_app.wifi_filter, &s_wifi_filt, frame)) {
            wifi_offer_frame(frame);
        }
    } else if (g_app.mode == CANGO_MODE_BLE) {
        if (cango_filter_match(&g_app.ble_filter, &s_ble_filt, frame)) {
            ble_offer_frame(frame);
        }
    }
}
