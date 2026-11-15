#include "app.h"
#include "cango_batch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "ble";

/* 8a3c0001-1f2e-4b5a-9c0d-11c0a1000001 and the three characteristics. */
static const ble_uuid128_t UUID_SVC =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x01, 0x00, 0x3c, 0x8a);
static const ble_uuid128_t UUID_W =
    BLE_UUID128_INIT(0x02, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x02, 0x00, 0x3c, 0x8a);
static const ble_uuid128_t UUID_N =
    BLE_UUID128_INIT(0x03, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x03, 0x00, 0x3c, 0x8a);
static const ble_uuid128_t UUID_D =
    BLE_UUID128_INIT(0x04, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x04, 0x00, 0x3c, 0x8a);

static uint16_t s_write_handle;
static uint16_t s_ctrl_handle;
static uint16_t s_data_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_ctrl_on;
static bool s_data_on;
static bool s_up;
static QueueHandle_t s_txq;
static char s_acc[800];
static size_t s_acc_n;

static void notify(uint16_t handle, const void *data, size_t len)
{
    struct os_mbuf *om;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || data == NULL || len == 0) {
        return;
    }
    om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn, handle, om);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv;
    const char *name = "CANGO";
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &UUID_SVC;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (uint8_t *)name;
    fields.name_len = 5;
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields %d", rc);
        fields.name = NULL;
        fields.name_len = 0;
        fields.name_is_complete = 0;
        rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGW(TAG, "adv uuid %d", rc);
        }
        memset(&fields, 0, sizeof(fields));
        fields.name = (uint8_t *)name;
        fields.name_len = 5;
        fields.name_is_complete = 1;
        ble_gap_adv_rsp_set_fields(&fields);
    }
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ctrl_on = false;
        s_data_on = false;
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ctrl_handle) {
            s_ctrl_on = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_data_handle) {
            s_data_on = event->subscribe.cur_notify;
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        return 0;
    default:
        return 0;
    }
}

static void take_write(struct os_mbuf *om)
{
    char out[1000];
    while (om != NULL && s_acc_n < sizeof(s_acc) - 1) {
        uint16_t n = om->om_len;
        if (s_acc_n + n >= sizeof(s_acc)) {
            n = (uint16_t)(sizeof(s_acc) - 1 - s_acc_n);
        }
        memcpy(s_acc + s_acc_n, om->om_data, n);
        s_acc_n += n;
        om = SLIST_NEXT(om, om_next);
    }
    s_acc[s_acc_n] = 0;
    if (strchr(s_acc, '\n') == NULL && s_acc_n < sizeof(s_acc) - 1) {
        return;
    }
    {
        int n = session_handle(s_acc, out, (int)sizeof(out), LINK_BLE);
        s_acc_n = 0;
        s_acc[0] = 0;
        if (n > 0 && s_ctrl_on) {
            notify(s_ctrl_handle, out, (size_t)n);
        }
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        take_write(ctxt->om);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &UUID_W.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_write_handle,
            },
            {
                .uuid = &UUID_N.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ctrl_handle,
            },
            {
                .uuid = &UUID_D.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_data_handle,
            },
            {0},
        },
    },
    {0},
};

static void on_sync(void)
{
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void tx_task(void *arg)
{
    int64_t last = 0;
    (void)arg;
    for (;;) {
        cango_frame_t batch[8];
        uint8_t n = 0;
        uint8_t raw[6 + 8 * 20];
        static uint16_t seq;
        int len;
        if (!s_up) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        while (n < 8 && xQueueReceive(s_txq, &batch[n], pdMS_TO_TICKS(n ? 0 : 200)) == pdTRUE) {
            n++;
        }
        if (n > 0 && s_data_on) {
            len = cango_batch_encode(raw, sizeof(raw), seq++, batch, n);
            if (len > 0) {
                notify(s_data_handle, raw, (size_t)len);
            }
        }
        if (esp_timer_get_time() - last > 1000000 && s_ctrl_on) {
            char status[512];
            session_status(status, (int)sizeof(status));
            notify(s_ctrl_handle, status, strlen(status));
            last = esp_timer_get_time();
        }
    }
}

void ble_offer_frame(const cango_frame_t *frame)
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

void ble_mode_start(void)
{
    if (s_up) {
        return;
    }
    if (s_txq == NULL) {
        s_txq = xQueueCreate(32, sizeof(cango_frame_t));
        xTaskCreate(tx_task, "ble_tx", 4096, NULL, 6, NULL);
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init");
        return;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("CANGO");
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    s_up = true;
    ESP_LOGI(TAG, "advertising");
}

void ble_mode_stop(void)
{
    if (!s_up) {
        return;
    }
    s_up = false;
    s_ctrl_on = false;
    s_data_on = false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
}
