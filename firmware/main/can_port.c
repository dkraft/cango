#include "app.h"
#include "board.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "can";

static QueueHandle_t s_frames;
static QueueHandle_t s_cmds;

typedef struct {
    int kind; /* 1 listen, 2 normal, 3 send */
    uint32_t bitrate;
    cango_frame_t frame;
} can_cmd_t;

QueueHandle_t can_frame_queue(void)
{
    return s_frames;
}

static twai_timing_config_t timing_for(uint32_t bitrate)
{
    if (bitrate <= 125000) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
        return t;
    }
    if (bitrate <= 250000) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
        return t;
    }
    if (bitrate >= 1000000) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
        return t;
    }
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    return t;
}

static void install(twai_mode_t mode, uint32_t bitrate)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(PIN_CAN_TX, PIN_CAN_RX, mode);
    twai_timing_config_t t = timing_for(bitrate);
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    static bool up;
    g.rx_queue_len = 64;
    g.tx_queue_len = 8;
    if (up) {
        twai_stop();
        twai_driver_uninstall();
        up = false;
    }
    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        ESP_LOGE(TAG, "install failed");
        return;
    }
    if (twai_start() != ESP_OK) {
        ESP_LOGE(TAG, "start failed");
        twai_driver_uninstall();
        return;
    }
    up = true;
    app_lock();
    g_app.listen_only = mode == TWAI_MODE_LISTEN_ONLY;
    g_app.bitrate = bitrate;
    snprintf(g_app.bus_state, sizeof(g_app.bus_state), "running");
    app_unlock();
}

static void can_task(void *arg)
{
    (void)arg;
    install(TWAI_MODE_LISTEN_ONLY, g_app.bitrate ? g_app.bitrate : 500000);
    for (;;) {
        can_cmd_t cmd;
        twai_message_t msg;
        while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
            if (cmd.kind == 1) {
                install(TWAI_MODE_LISTEN_ONLY, cmd.bitrate);
            } else if (cmd.kind == 2) {
                install(TWAI_MODE_NORMAL, cmd.bitrate);
            } else if (cmd.kind == 3 && !g_app.listen_only) {
                memset(&msg, 0, sizeof(msg));
                msg.identifier = cmd.frame.id;
                msg.extd = (cmd.frame.flags & CANGO_F_EXT) ? 1 : 0;
                msg.rtr = (cmd.frame.flags & CANGO_F_RTR) ? 1 : 0;
                msg.data_length_code = cmd.frame.len > 8 ? 8 : cmd.frame.len;
                memcpy(msg.data, cmd.frame.data, msg.data_length_code);
                if (twai_transmit(&msg, pdMS_TO_TICKS(20)) != ESP_OK) {
                    ESP_LOGW(TAG, "tx failed");
                }
            }
        }
        if (twai_receive(&msg, pdMS_TO_TICKS(10)) == ESP_OK) {
            cango_frame_t f;
            twai_status_info_t st;
            memset(&f, 0, sizeof(f));
            f.t_us = (uint32_t)esp_timer_get_time();
            f.id = msg.identifier;
            f.len = msg.data_length_code > 8 ? 8 : msg.data_length_code;
            if (msg.extd) {
                f.flags |= CANGO_F_EXT;
            }
            if (msg.rtr) {
                f.flags |= CANGO_F_RTR;
            }
            memcpy(f.data, msg.data, f.len);
            if (xQueueSend(s_frames, &f, 0) != pdTRUE) {
                app_lock();
                g_app.missed++;
                app_unlock();
            } else {
                app_lock();
                g_app.rx_count++;
                app_unlock();
            }
            if (twai_get_status_info(&st) == ESP_OK) {
                app_lock();
                g_app.tec = st.tx_error_counter;
                g_app.rec = st.rx_error_counter;
                if (st.state == TWAI_STATE_BUS_OFF) {
                    snprintf(g_app.bus_state, sizeof(g_app.bus_state), "bus_off");
                } else if (st.state == TWAI_STATE_RECOVERING) {
                    snprintf(g_app.bus_state, sizeof(g_app.bus_state), "recovering");
                } else {
                    snprintf(g_app.bus_state, sizeof(g_app.bus_state), "running");
                }
                app_unlock();
            }
        }
    }
}

void can_port_start(void)
{
    s_frames = xQueueCreate(256, sizeof(cango_frame_t));
    s_cmds = xQueueCreate(8, sizeof(can_cmd_t));
    xTaskCreatePinnedToCore(can_task, "can", 4096, NULL, 20, NULL, 1);
}

static void push_cmd(can_cmd_t cmd)
{
    if (s_cmds != NULL) {
        xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(50));
    }
}

void can_request_listen(uint32_t bitrate)
{
    can_cmd_t cmd = {.kind = 1, .bitrate = bitrate};
    push_cmd(cmd);
}

void can_request_normal(uint32_t bitrate)
{
    can_cmd_t cmd = {.kind = 2, .bitrate = bitrate};
    push_cmd(cmd);
}

bool can_request_send(const cango_frame_t *frame)
{
    can_cmd_t cmd = {.kind = 3};
    if (frame == NULL || s_cmds == NULL) {
        return false;
    }
    cmd.frame = *frame;
    cmd.frame.flags = (uint8_t)(cmd.frame.flags | CANGO_F_TX);
    return xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(50)) == pdTRUE;
}
