#include "app.h"
#include "board.h"
#include "led.h"
#include "storage.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

cango_app_t g_app;
static SemaphoreHandle_t s_mu;

uint32_t app_now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

void app_lock(void)
{
    if (s_mu != NULL) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

void app_unlock(void)
{
    if (s_mu != NULL) {
        xSemaphoreGive(s_mu);
    }
}

void app_main(void)
{
    memset(&g_app, 0, sizeof(g_app));
    snprintf(g_app.bus_state, sizeof(g_app.bus_state), "down");
    snprintf(g_app.ap_ssid, sizeof(g_app.ap_ssid), "CANGO");
    g_app.bitrate = 500000;
    g_app.listen_only = true;
    s_mu = xSemaphoreCreateMutex();
    board_init();
    led_init();
    storage_init();
    storage_load_sta();
    storage_seed();
    storage_load_logging();
    storage_load_project();
    storage_load_mode();
    /* Classic BT is unused. Leave BLE memory in place; WiFi and BLE start later. */
    if (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGW("cango", "classic bt mem release skipped");
    }
    can_port_start();
    ctrl_start();
    vTaskDelay(pdMS_TO_TICKS(800));
    ctrl_boot_release();
    ESP_LOGI("cango", "up mode %u profile %u", g_app.mode, g_app.logging.profile);
}
