#include "app.h"
#include "board.h"
#include "led.h"
#include "storage.h"

#include "cango_log.h"
#include "cango_mode.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ctrl";

static cango_capture_t *s_cap;
static cango_debounce_t s_btn;
static FILE *s_startup_fp;
static FILE *s_dealer_fp;
static cango_log_t *s_startup;
static cango_log_t *s_dealer;
static char s_startup_path[128];
static bool s_complete;
static bool s_running;
static QueueHandle_t s_user_q;

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

static cango_log_t *open_log(FILE **fp, const char *dir, uint32_t stamp, char *path, int path_cap, uint8_t profile)
{
    cango_log_info_t info;
    *fp = storage_open_log(dir, stamp, path, path_cap);
    if (*fp == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.bitrate = g_app.bitrate;
    info.listen_only = 1;
    info.profile = profile;
    memcpy(info.project_id, g_app.project.id, sizeof(info.project_id) - 1);
    info.start_us = stamp;
    info.unix_s = session_unix_s();
    return cango_log_open(file_write, file_write, *fp, &info);
}

static void close_fp(cango_log_t **log, FILE **fp)
{
    if (*log != NULL) {
        cango_log_close(*log);
        *log = NULL;
    }
    if (*fp != NULL) {
        fclose(*fp);
        *fp = NULL;
    }
}

static void on_cap(void *ctx, int action, const cango_frame_t *frame, uint32_t aux)
{
    (void)ctx;
    if (action == CANGO_CAP_OPEN_STARTUP) {
        close_fp(&s_startup, &s_startup_fp);
        s_startup = open_log(&s_startup_fp, "/sdcard/logs/startup", aux, s_startup_path, (int)sizeof(s_startup_path),
                             CANGO_PROFILE_STARTUP);
    } else if (action == CANGO_CAP_OPEN_DEALER) {
        char path[128];
        close_fp(&s_dealer, &s_dealer_fp);
        s_dealer = open_log(&s_dealer_fp, "/sdcard/logs/dealer", aux, path, (int)sizeof(path), CANGO_PROFILE_DEALER);
    } else if (action == CANGO_CAP_OPEN_MANUAL) {
        char path[128];
        FILE *fp = NULL;
        cango_log_t *log = open_log(&fp, "/sdcard/logs/manual", aux, path, (int)sizeof(path), CANGO_PROFILE_MANUAL);
        if (log != NULL) {
            cango_log_user_event(log, aux);
            cango_log_close(log);
        }
        if (fp != NULL) {
            fclose(fp);
        }
    } else if (action == CANGO_CAP_WRITE_FRAME && frame != NULL) {
        if (aux == CANGO_DEST_STARTUP && s_startup != NULL) {
            cango_log_frame(s_startup, frame);
        }
        if (aux == CANGO_DEST_DEALER && s_dealer != NULL) {
            cango_log_frame(s_dealer, frame);
        }
    } else if (action == CANGO_CAP_WRITE_EVENT) {
        if (s_startup != NULL) {
            cango_log_user_event(s_startup, aux);
            cango_log_flush(s_startup);
        }
        if (s_dealer != NULL) {
            cango_log_user_event(s_dealer, aux);
            cango_log_flush(s_dealer);
        }
    } else if (action == CANGO_CAP_MARKER) {
        if (s_startup != NULL) {
            cango_log_marker(s_startup, (uint8_t)aux, app_now_us());
        }
        if (s_dealer != NULL) {
            cango_log_marker(s_dealer, (uint8_t)aux, app_now_us());
        }
    } else if (action == CANGO_CAP_CLOSE_STARTUP) {
        if (s_startup != NULL && s_cap != NULL) {
            char json[2048];
            const cango_census_t *census = cango_capture_census(s_cap);
            if (census != NULL && cango_census_json(census, json, (int)sizeof(json)) > 0) {
                storage_write_census(s_startup_path, json);
            }
        }
        close_fp(&s_startup, &s_startup_fp);
    } else if (action == CANGO_CAP_CLOSE_DEALER) {
        close_fp(&s_dealer, &s_dealer_fp);
    } else if (action == CANGO_CAP_COMPLETE) {
        s_complete = true;
        s_running = false;
    }
}

static void capture_stop(void)
{
    close_fp(&s_startup, &s_startup_fp);
    close_fp(&s_dealer, &s_dealer_fp);
    if (s_cap != NULL) {
        cango_capture_free(s_cap);
        s_cap = NULL;
    }
    s_running = false;
}

static void capture_start(void)
{
    capture_stop();
    s_complete = false;
    s_cap = cango_capture_new(&g_app.logging);
    if (s_cap == NULL) {
        return;
    }
    s_running = true;
    cango_capture_begin(s_cap, app_now_us(), on_cap, NULL);
}

bool ctrl_capture_running(void)
{
    return s_running;
}

bool ctrl_capture_complete(void)
{
    return s_complete;
}

static void paint(uint32_t now_ms)
{
    bool on = ((now_ms / 150) % 2) == 0;
    if (!g_app.boot_done) {
        led_set(on ? 80 : 0, 0, 0);
        return;
    }
    if (g_app.mode == CANGO_MODE_CAPTURE) {
        if (s_complete) {
            led_set(80, 80, 80);
        } else {
            led_set(60, 0, 80);
        }
        return;
    }
    if (g_app.mode == CANGO_MODE_BLE) {
        led_set(0, 0, on ? 80 : 0);
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
        capture_stop();
    }
    g_app.mode = mode;
    storage_save_mode(mode);
    g_app.sta_up = false;
    if (mode == CANGO_MODE_CAPTURE) {
        capture_start();
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
        uint32_t stamp;
        if (g_app.boot_done) {
            ev = cango_debounce_feed(&s_btn, down, now_ms);
        }
        if (ev == CANGO_BTN_LONG) {
            enter_mode(cango_mode_next(g_app.mode));
        } else if (ev == CANGO_BTN_SHORT) {
            stamp = app_now_us();
            xQueueSend(s_user_q, &stamp, 0);
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
        uint32_t user;
        if (frames != NULL && xQueueReceive(frames, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_app.mode == CANGO_MODE_CAPTURE && s_cap != NULL && !s_complete) {
                cango_capture_on_frame(s_cap, &frame, on_cap, NULL);
            } else if (g_app.mode != CANGO_MODE_CAPTURE) {
                session_offer_frame(&frame);
            }
        }
        if (s_cap != NULL && g_app.mode == CANGO_MODE_CAPTURE && !s_complete) {
            cango_capture_poll(s_cap, app_now_us(), on_cap, NULL);
        }
        while (xQueueReceive(s_user_q, &user, 0) == pdTRUE) {
            if (g_app.mode == CANGO_MODE_CAPTURE && s_cap != NULL) {
                cango_capture_on_user(s_cap, user, on_cap, NULL);
            }
        }
    }
}

void ctrl_start(void)
{
    cango_debounce_init(&s_btn);
    s_user_q = xQueueCreate(8, sizeof(uint32_t));
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
