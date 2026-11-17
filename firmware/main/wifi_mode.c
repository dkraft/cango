#include "app.h"
#include "cango_batch.h"
#include "cango_path.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void copy_fit(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (src[i] != '\0' && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static const char *TAG = "wifi";
static httpd_handle_t s_httpd;
static esp_netif_t *s_ap;
static esp_netif_t *s_sta;
static bool s_up;
static int s_try;
static QueueHandle_t s_txq;
static int s_clients[4];
static int s_nclients;

static const char *FALLBACK =
    "<!doctype html><meta charset=utf-8><title>CANGO</title>"
    "<body style=\"font:14px ui-monospace,monospace;background:#14140f;color:#e6e1d6\">"
    "<h1>CANGO</h1><p>The SD card has no /www page. Copy the web UI onto the card.</p>"
    "<pre id=s></pre><script>"
    "const s=document.getElementById('s');"
    "const w=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');"
    "w.onopen=()=>w.send(JSON.stringify({op:'hello'}));"
    "w.onmessage=e=>{if(typeof e.data=='string')s.textContent=e.data;};"
    "</script>";

static void remember_client(int fd)
{
    int i;
    for (i = 0; i < s_nclients; i++) {
        if (s_clients[i] == fd) {
            return;
        }
    }
    if (s_nclients < 4) {
        s_clients[s_nclients++] = fd;
    }
}

static void drop_client(int fd)
{
    int i;
    for (i = 0; i < s_nclients; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = s_clients[s_nclients - 1];
            s_nclients--;
            return;
        }
    }
}

static void send_clients(const uint8_t *data, size_t len, httpd_ws_type_t type)
{
    int i;
    for (i = 0; i < s_nclients; i++) {
        httpd_ws_frame_t frame = {
            .final = true,
            .type = type,
            .payload = (uint8_t *)data,
            .len = len,
        };
        if (httpd_ws_send_frame_async(s_httpd, s_clients[i], &frame) != ESP_OK) {
            drop_client(s_clients[i]);
            i--;
        }
    }
}

static esp_err_t ws_get(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        remember_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t in = {0};
    uint8_t *buf;
    char out[1200];
    esp_err_t err = httpd_ws_recv_frame(req, &in, 0);
    if (err != ESP_OK || in.len == 0 || in.len > 1000) {
        return err;
    }
    buf = calloc(1, in.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    in.payload = buf;
    err = httpd_ws_recv_frame(req, &in, in.len);
    if (err == ESP_OK && in.type == HTTPD_WS_TYPE_TEXT) {
        int n = session_handle((char *)buf, out, (int)sizeof(out), LINK_WIFI);
        if (n > 0) {
            httpd_ws_frame_t reply = {
                .final = true,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)out,
                .len = (size_t)n,
            };
            httpd_ws_send_frame(req, &reply);
        }
    }
    free(buf);
    return ESP_OK;
}

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(dot, ".js") == 0) {
        return "text/javascript";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(dot, ".cango") == 0) {
        return "application/octet-stream";
    }
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    char chunk[1024];
    size_t n;
    if (f == NULL) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type(path));
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_get(httpd_req_t *req)
{
    char path[160];
    char uri[96];
    char *q;
    copy_fit(uri, sizeof(uri), req->uri);
    q = strchr(uri, '?');
    if (q != NULL) {
        *q = 0;
    }
    if (strcmp(uri, "/") == 0) {
        snprintf(uri, sizeof(uri), "/index.html");
    }
    if (strstr(uri, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path");
        return ESP_FAIL;
    }
    snprintf(path, sizeof(path), "/sdcard/www%s", uri);
    if (send_file(req, path) == ESP_OK) {
        return ESP_OK;
    }
    if (strcmp(uri, "/index.html") == 0) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, FALLBACK, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
    return ESP_FAIL;
}

static int hex_val(char c)
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

static bool url_path(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (dst == NULL || cap == 0) {
        return false;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return true;
    }
    while (*src != '\0' && *src != '?' && *src != '#') {
        char c = *src++;
        if (c == '%') {
            int hi = hex_val(*src);
            int lo = hex_val(src[1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            src += 2;
            c = (char)((hi << 4) | lo);
        }
        if (i + 1 >= cap) {
            return false;
        }
        dst[i++] = c;
    }
    dst[i] = '\0';
    return true;
}

static esp_err_t sd_get(httpd_req_t *req)
{
    char rel[128];
    char path[192];
    char disp[80];
    const char *base;
    struct stat st;
    const char *rest = req->uri + strlen("/sd/");
    if (!url_path(rel, sizeof(rel), rest) || !cango_path_under(path, sizeof(path), "/sdcard", rel)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path");
        return ESP_FAIL;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    {
        static const char prefix[] = "attachment; filename=\"";
        size_t nb = strlen(base);
        size_t np = sizeof(prefix) - 1;
        if (nb > 47) {
            nb = 47;
        }
        memcpy(disp, prefix, np);
        memcpy(disp + np, base, nb);
        disp[np + nb] = '"';
        disp[np + nb + 1] = '\0';
    }
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    if (send_file(req, path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t logs_get(httpd_req_t *req)
{
    char path[160];
    const char *rest = req->uri + strlen("/logs/");
    if (strstr(rest, "..") != NULL || strchr(rest, '?') != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path");
        return ESP_FAIL;
    }
    snprintf(path, sizeof(path), "/sdcard/logs/%s", rest);
    if (send_file(req, path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_get, .is_websocket = true};
    httpd_uri_t logs = {.uri = "/logs/*", .method = HTTP_GET, .handler = logs_get};
    httpd_uri_t sd = {.uri = "/sd/*", .method = HTTP_GET, .handler = sd_get};
    httpd_uri_t page = {.uri = "/*", .method = HTTP_GET, .handler = page_get};
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd");
        return;
    }
    httpd_register_uri_handler(s_httpd, &ws);
    httpd_register_uri_handler(s_httpd, &logs);
    httpd_register_uri_handler(s_httpd, &sd);
    httpd_register_uri_handler(s_httpd, &page);
}

static void connect_slot(int slot)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (slot < 0 || slot >= g_app.sta_n) {
        return;
    }
    copy_fit((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), g_app.sta[slot].ssid);
    copy_fit((char *)cfg.sta.password, sizeof(cfg.sta.password), g_app.sta[slot].pass);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    ESP_LOGI(TAG, "sta try %s", g_app.sta[slot].ssid);
}

static bool ap_has(const wifi_ap_record_t *recs, uint16_t n, const char *ssid)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        if (strncmp((const char *)recs[i].ssid, ssid, 32) == 0) {
            return true;
        }
    }
    return false;
}

static void scan_and_pick(void)
{
    wifi_scan_config_t scan = {0};
    esp_wifi_scan_start(&scan, false);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        wifi_ap_record_t *recs;
        int i;
        esp_wifi_scan_get_ap_num(&n);
        recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs == NULL) {
            return;
        }
        esp_wifi_scan_get_ap_records(&n, recs);
        for (i = 0; i < g_app.sta_n; i++) {
            int slot = (s_try + i) % g_app.sta_n;
            if (ap_has(recs, n, g_app.sta[slot].ssid)) {
                s_try = slot;
                connect_slot(slot);
                break;
            }
        }
        free(recs);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_app.sta_up = false;
        if (g_app.sta_n > 0) {
            s_try = (s_try + 1) % g_app.sta_n;
            scan_and_pick();
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    g_app.sta_up = true;
    ESP_LOGI(TAG, "sta has lan");
}

static void tx_task(void *arg)
{
    int64_t last_status = 0;
    (void)arg;
    for (;;) {
        cango_frame_t batch[12];
        uint8_t n = 0;
        uint8_t raw[6 + 12 * 20];
        int len;
        static uint16_t seq;
        int64_t now;
        if (!s_up) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        while (n < 12 && xQueueReceive(s_txq, &batch[n], pdMS_TO_TICKS(n ? 0 : 200)) == pdTRUE) {
            n++;
        }
        if (n > 0) {
            len = cango_batch_encode(raw, sizeof(raw), seq++, batch, n);
            if (len > 0) {
                send_clients(raw, (size_t)len, HTTPD_WS_TYPE_BINARY);
            }
        }
        now = esp_timer_get_time();
        {
            char late[160];
            int late_n = session_poll(LINK_WIFI, late, (int)sizeof(late));
            if (late_n > 0) {
                send_clients((const uint8_t *)late, (size_t)late_n, HTTPD_WS_TYPE_TEXT);
            }
        }
        if (now - last_status > 1000000) {
            char status[512];
            session_status(status, (int)sizeof(status));
            send_clients((const uint8_t *)status, strlen(status), HTTPD_WS_TYPE_TEXT);
            last_status = now;
        }
    }
}

void wifi_offer_frame(const cango_frame_t *frame)
{
    if (s_txq == NULL || frame == NULL) {
        return;
    }
    if (xQueueSend(s_txq, frame, 0) != pdTRUE) {
        app_lock();
        g_app.dropped++;
        app_unlock();
    }
}

void wifi_mode_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap;
    uint8_t mac[6];
    if (s_up) {
        return;
    }
    if (s_txq == NULL) {
        s_txq = xQueueCreate(64, sizeof(cango_frame_t));
        xTaskCreate(tx_task, "wifi_tx", 6144, NULL, 6, NULL);
    }
    static bool loop_ready;
    esp_netif_init();
    if (!loop_ready) {
        esp_event_loop_create_default();
        loop_ready = true;
    }
    s_ap = esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_wifi_init(&init);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_app.ap_ssid, sizeof(g_app.ap_ssid), "CANGO-%02X%02X", mac[4], mac[5]);
    memset(&ap, 0, sizeof(ap));
    copy_fit((char *)ap.ap.ssid, sizeof(ap.ap.ssid), g_app.ap_ssid);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    start_httpd();
    s_up = true;
    g_app.sta_up = false;
    s_try = 0;
    if (g_app.sta_n > 0) {
        scan_and_pick();
    }
    gvret_start();
    ESP_LOGI(TAG, "ap %s", g_app.ap_ssid);
}

void wifi_mode_stop(void)
{
    if (!s_up) {
        return;
    }
    gvret_stop();
    s_up = false;
    g_app.sta_up = false;
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_nclients = 0;
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip);
    if (s_ap != NULL) {
        esp_netif_destroy(s_ap);
        s_ap = NULL;
    }
    if (s_sta != NULL) {
        esp_netif_destroy(s_sta);
        s_sta = NULL;
    }
}
