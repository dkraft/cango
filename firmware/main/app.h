#ifndef APP_H
#define APP_H

#include "cango_capture.h"
#include "cango_filter.h"
#include "cango_frame.h"
#include "cango_log.h"
#include "cango_mode.h"
#include "cango_project.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdbool.h>
#include <stdint.h>

#define LINK_WIFI 1
#define LINK_BLE 2
#define CANGO_STA_MAX 4
#define CANGO_SSID_LEN 33
#define CANGO_PASS_LEN 65

typedef struct {
    char ssid[CANGO_SSID_LEN];
    char pass[CANGO_PASS_LEN];
} cango_sta_t;

typedef struct {
    uint8_t mode;
    bool boot_done;
    bool sd_ok;
    bool sta_up;
    bool armed;
    uint32_t arm_until_us;
    bool listen_only;
    uint32_t bitrate;
    uint32_t rx_count;
    uint32_t missed;
    uint32_t dropped;
    uint32_t tec;
    uint32_t rec;
    char bus_state[16];
    cango_project_t project;
    cango_capture_cfg_t logging;
    cango_filter_t wifi_filter;
    cango_filter_t ble_filter;
    int sta_n;
    cango_sta_t sta[CANGO_STA_MAX];
    char ap_ssid[CANGO_SSID_LEN];
} cango_app_t;

extern cango_app_t g_app;

uint32_t app_now_us(void);
void app_lock(void);
void app_unlock(void);

/* CAN commands, processed only on the CAN task. */
void can_port_start(void);
QueueHandle_t can_frame_queue(void);
void can_request_listen(uint32_t bitrate);
void can_request_normal(uint32_t bitrate);
bool can_request_send(const cango_frame_t *frame);

/* Mode entry. Safe to call from the control task. */
void wifi_mode_start(void);
void wifi_mode_stop(void);
void ble_mode_start(void);
void ble_mode_stop(void);
bool ble_link_up(void);
void ctrl_start(void);
void ctrl_boot_release(void);
/* Copies the open capture directory, or writes an empty string. */
void ctrl_capture_dir(char *out, int cap);
void wifi_offer_frame(const cango_frame_t *frame);
void ble_offer_frame(const cango_frame_t *frame);
void ble_heard_reset(void);
void gvret_start(void);
void gvret_stop(void);
void gvret_offer_frame(const cango_frame_t *frame);

void session_offer_frame(const cango_frame_t *frame);
int session_handle(const char *json, char *out, int cap, int link);
void session_status(char *out, int cap);
/* A finished format_card line for this link, or 0 when none is waiting. */
int session_poll(int link, char *out, int cap);
uint64_t session_unix_s(void);

#endif
