#include "app.h"
#include "board.h"
#include "led.h"
#include "storage.h"

#include "cango_capture.h"
#include "cango_log.h"
#include "cango_mode.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ctrl";

#define CAPTURE_WINDOW_US (30u * 1000000u)
#define CMD_BEGIN 1
#define CMD_END 2

static cango_debounce_t s_btn;
static FILE *s_fp;
static FILE *s_csv;
static cango_log_t *s_log;
static char s_dir[96];
static cango_census_t s_census;
static uint32_t s_end_us;
static volatile bool s_running;
static volatile bool s_complete;
static QueueHandle_t s_cmd_q;

static int file_write(void *ctx, const void *data, size_t len)
{
    FILE *f = ctx;
    if (f == NULL) {
        return -1;
    }
    if (data == NULL || len == 0) {
        fflush(f);
        return 0;
    }
    return fwrite(data, 1, len, f) == len ? 0 : -1;
}

static void queue_cmd(uint8_t cmd)
{
    if (s_cmd_q != NULL) {
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

static void csv_close(void)
{
    if (s_csv != NULL) {
        fclose(s_csv);
        s_csv = NULL;
    }
}

static void csv_open(void)
{
    char path[128];
    csv_close();
    if (s_dir[0] == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/log.csv", s_dir);
    s_csv = fopen(path, "w");
    if (s_csv != NULL) {
        fputs("Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n", s_csv);
    }
}

static void csv_frame(const cango_frame_t *frame)
{
    uint8_t i;
    uint8_t n;
    if (s_csv == NULL || frame == NULL) {
        return;
    }
    n = frame->len > 8 ? 8 : frame->len;
    fprintf(s_csv, "%" PRIu32 ",%08" PRIX32 ",%s,0,%u", frame->t_us, frame->id & 0x1fffffffu,
            (frame->flags & CANGO_F_EXT) != 0 ? "true" : "false", (unsigned)n);
    for (i = 0; i < 8; i++) {
        fprintf(s_csv, ",%02X", i < n ? frame->data[i] : 0);
    }
    fputs(",\n", s_csv);
}

static void capture_close(bool done)
{
    csv_close();
    if (s_log != NULL) {
        char json[2048];
        char path[128];
        if (s_dir[0] != 0 && cango_census_json(&s_census, json, (int)sizeof(json)) > 0) {
            snprintf(path, sizeof(path), "%s/log.cango", s_dir);
            storage_write_census(path, json);
        }
        cango_log_close(s_log);
        s_log = NULL;
    }
    if (s_fp != NULL) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_running = false;
    if (done) {
        s_complete = true;
    }
}

/* One folder, every frame, thirty seconds. A missing card records nothing. */
static void capture_begin(uint32_t now_us)
{
    cango_log_info_t info;
    capture_close(false);
    s_complete = false;
    s_dir[0] = 0;
    s_fp = storage_open_capture(now_us, s_dir, (int)sizeof(s_dir));
    if (s_fp == NULL) {
        if (g_app.sd_ok) {
            ESP_LOGW(TAG, "log open failed");
        }
        return;
    }
    memset(&s_census, 0, sizeof(s_census));
    memset(&info, 0, sizeof(info));
    info.bitrate = g_app.bitrate;
    info.listen_only = 1;
    info.profile = CANGO_PROFILE_WINDOW;
    memcpy(info.project_id, g_app.project.id, sizeof(info.project_id) - 1);
    info.start_us = now_us;
    info.unix_s = session_unix_s();
    s_log = cango_log_open(file_write, file_write, s_fp, &info);
    if (s_log == NULL) {
        fclose(s_fp);
        s_fp = NULL;
        return;
    }
    s_end_us = now_us + CAPTURE_WINDOW_US;
    s_running = true;
    csv_open();
    ESP_LOGI(TAG, "log %s for 30s", s_dir);
}

static void capture_poll(uint32_t now_us)
{
    uint8_t cmd;
    while (s_cmd_q != NULL && xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
        if (cmd == CMD_BEGIN) {
            capture_begin(now_us);
        } else if (cmd == CMD_END) {
            capture_close(false);
            s_complete = false;
        }
    }
    if (s_running && (int32_t)(now_us - s_end_us) >= 0) {
        capture_close(true);
        ESP_LOGI(TAG, "log closed");
    }
}

bool ctrl_capture_running(void)
{
    return s_running;
}

bool ctrl_capture_complete(void)
{
    return s_complete;
}

void ctrl_capture_dir(char *out, int cap)
{
    if (out == NULL || cap < 2) {
        return;
    }
    out[0] = '\0';
    if (!s_running || s_dir[0] == '\0') {
        return;
    }
    snprintf(out, (size_t)cap, "%s", s_dir);
}

static void paint(uint32_t now_ms)
{
    bool on = ((now_ms / 150) % 2) == 0;
    if (!g_app.boot_done) {
        led_set(on ? 80 : 0, 0, 0);
        return;
    }
    if (g_app.mode == CANGO_MODE_CAPTURE) {
        if (s_running) {
            led_set(on ? 60 : 0, 0, on ? 80 : 0);
        } else {
            led_set(60, 0, 80);
        }
        return;
    }
    if (g_app.mode == CANGO_MODE_BLE) {
        if (ble_link_up()) {
            led_set(0, 0, 80);
        } else {
            led_set(0, 0, on ? 80 : 0);
        }
        return;
    }
    if (g_app.sta_up) {
        led_set(0, 80, 0);
    } else {
        led_set(0, on ? 80 : 0, 0);
    }
}

static void enter_mode(uint8_t mode)
{
    if (g_app.mode == CANGO_MODE_WIFI && mode != CANGO_MODE_WIFI) {
        wifi_mode_stop();
    }
    if (g_app.mode == CANGO_MODE_BLE && mode != CANGO_MODE_BLE) {
        ble_mode_stop();
    }
    if (g_app.mode == CANGO_MODE_CAPTURE && mode != CANGO_MODE_CAPTURE) {
        queue_cmd(CMD_END);
    }
    g_app.mode = mode;
    storage_save_mode(mode);
    g_app.sta_up = false;
    if (mode == CANGO_MODE_CAPTURE) {
        queue_cmd(CMD_BEGIN);
    } else if (mode == CANGO_MODE_BLE) {
        ble_mode_start();
    } else {
        wifi_mode_start();
    }
    ESP_LOGI(TAG, "mode %u", mode);
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool down = gpio_get_level(PIN_BOOT) == 0;
        int ev = CANGO_BTN_NONE;
        if (g_app.boot_done) {
            ev = cango_debounce_feed(&s_btn, down, now_ms);
        }
        if (ev == CANGO_BTN_LONG) {
            enter_mode(cango_mode_next(g_app.mode));
        } else if (ev == CANGO_BTN_SHORT && g_app.mode == CANGO_MODE_CAPTURE) {
            queue_cmd(CMD_BEGIN);
        }
        if (g_app.armed && app_now_us() > g_app.arm_until_us) {
            g_app.armed = false;
            can_request_listen(g_app.bitrate);
        }
        paint(now_ms);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void log_task(void *arg)
{
    QueueHandle_t frames = can_frame_queue();
    (void)arg;
    for (;;) {
        cango_frame_t frame;
        capture_poll(app_now_us());
        if (frames != NULL && xQueueReceive(frames, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_app.mode == CANGO_MODE_CAPTURE && s_running && s_log != NULL) {
                cango_log_frame(s_log, &frame);
                csv_frame(&frame);
                cango_census_add(&s_census, &frame);
            } else if (g_app.mode != CANGO_MODE_CAPTURE) {
                session_offer_frame(&frame);
            }
        }
    }
}

void ctrl_start(void)
{
    cango_debounce_init(&s_btn);
    s_cmd_q = xQueueCreate(8, sizeof(uint8_t));
    xTaskCreatePinnedToCore(log_task, "log", 8192, NULL, 12, NULL, 0);
    xTaskCreatePinnedToCore(ui_task, "ui", 4096, NULL, 8, NULL, 0);
}

void ctrl_boot_release(void)
{
    bool down = gpio_get_level(PIN_BOOT) == 0;
    s_btn.button.in_boot = true;
    cango_debounce_feed(&s_btn, down, (uint32_t)(esp_timer_get_time() / 1000));
    cango_debounce_end_boot(&s_btn);
    g_app.boot_done = true;
    enter_mode(g_app.mode);
}
