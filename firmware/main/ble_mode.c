#include "app.h"
#include "cango_batch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
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
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_alive_ms;
static volatile bool s_stale;
static bool s_ctrl_on;
static bool s_data_on;
static bool s_dropping;
static bool s_up;
static bool s_watch_on;
static struct ble_npl_callout s_watch;
static char s_acc[800];
static size_t s_acc_n;

#define BLE_HEARD 64
#define BLE_COALESCE_US 200000u

typedef struct {
    bool used;
    bool pending;
    uint32_t sent_us;
    cango_frame_t frame;
} ble_heard_t;

static ble_heard_t s_heard[BLE_HEARD];
static SemaphoreHandle_t s_heard_mu;
static bool s_tx_task;

static void heard_lock(void)
{
    if (s_heard_mu != NULL) {
        xSemaphoreTake(s_heard_mu, portMAX_DELAY);
    }
}

static void heard_unlock(void)
{
    if (s_heard_mu != NULL) {
        xSemaphoreGive(s_heard_mu);
    }
}

void ble_heard_reset(void)
{
    heard_lock();
    memset(s_heard, 0, sizeof(s_heard));
    heard_unlock();
}

static void notify(uint16_t handle, const void *data, size_t len)
{
    struct os_mbuf *om;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || data == NULL || len == 0) {
        return;
    }
    om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) {
        return;
    }
    if (ble_gatts_notify_custom(s_conn, handle, om) == BLE_HS_ENOTCONN) {
        s_stale = true;
    }
}

static void advertise(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void link_down(const char *why)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_ctrl_on && !s_data_on && !s_stale) {
        return;
    }
    ESP_LOGI(TAG, "link down %s, led flash", why);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_ctrl_on = false;
    s_data_on = false;
    s_dropping = false;
    s_stale = false;
    s_alive_ms = 0;
    advertise();
}

static void tighten_link(uint16_t conn)
{
    struct ble_gap_upd_params params;
    int rc;
    memset(&params, 0, sizeof(params));
    params.itvl_min = 24;
    params.itvl_max = 40;
    params.latency = 0;
    params.supervision_timeout = 200;
    rc = ble_gap_update_params(conn, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "conn params %d", rc);
    }
}

/* Host task. A handle that NimBLE has already dropped, or a subscribed
 * link with no successful notify for 8 seconds, is not a phone. */
static void watch_cb(struct ble_npl_event *ev)
{
    struct ble_gap_conn_desc desc;
    uint32_t now;
    (void)ev;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        now = now_ms();
        if (s_stale || ble_gap_conn_find(s_conn, &desc) != 0) {
            link_down(s_stale ? "notify" : "missing");
        } else if (!s_dropping && s_ctrl_on && s_alive_ms != 0 && (int32_t)(now - s_alive_ms) > 8000) {
            int rc;
            s_dropping = true;
            rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            ESP_LOGW(TAG, "link quiet terminate %d", rc);
            if (rc != 0) {
                link_down("quiet");
            }
        }
    }
    if (s_up) {
        ble_npl_callout_reset(&s_watch, ble_npl_time_ms_to_ticks32(1000));
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
    if (rc != 0 && rc != BLE_HS_EALREADY) {
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
            s_dropping = false;
            s_stale = false;
            s_alive_ms = now_ms();
            tighten_link(s_conn);
            ESP_LOGI(TAG, "link up, led solid");
        } else {
            ESP_LOGW(TAG, "link failed %d", event->connect.status);
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        link_down("reason");
        ESP_LOGI(TAG, "link reason %d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status == 0 || event->notify_tx.status == BLE_HS_EDONE) {
            s_alive_ms = now_ms();
        } else if (event->notify_tx.status == BLE_HS_ENOTCONN) {
            s_stale = true;
        } else {
            ESP_LOGW(TAG, "notify tx %d", event->notify_tx.status);
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ctrl_handle) {
            s_ctrl_on = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_data_handle) {
            s_data_on = event->subscribe.cur_notify;
        }
        s_alive_ms = now_ms();
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
    if (!s_watch_on) {
        ble_npl_callout_init(&s_watch, nimble_port_get_dflt_eventq(), watch_cb, NULL);
        s_watch_on = true;
    }
    ble_npl_callout_reset(&s_watch, ble_npl_time_ms_to_ticks32(1000));
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
        uint32_t now;
        int i;
        if (!s_up) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        now = app_now_us();
        heard_lock();
        for (i = 0; i < BLE_HEARD && n < 8; i++) {
            ble_heard_t *slot = &s_heard[i];
            if (!slot->used || !slot->pending) {
                continue;
            }
            if (slot->sent_us != 0 && (uint32_t)(now - slot->sent_us) < BLE_COALESCE_US) {
                continue;
            }
            batch[n++] = slot->frame;
            slot->pending = false;
            slot->sent_us = now;
        }
        heard_unlock();
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
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ble_offer_frame(const cango_frame_t *frame)
{
    int i;
    int free_i = -1;
    int quiet_i = -1;
    uint32_t quiet_age = 0;
    uint8_t ext;
    uint32_t now;
    if (frame == NULL || s_heard_mu == NULL) {
        return;
    }
    ext = (uint8_t)(frame->flags & CANGO_F_EXT);
    now = frame->t_us;
    heard_lock();
    for (i = 0; i < BLE_HEARD; i++) {
        if (s_heard[i].used && s_heard[i].frame.id == frame->id && (s_heard[i].frame.flags & CANGO_F_EXT) == ext) {
            s_heard[i].frame = *frame;
            s_heard[i].pending = true;
            heard_unlock();
            return;
        }
        if (!s_heard[i].used && free_i < 0) {
            free_i = i;
        } else if (s_heard[i].used) {
            uint32_t age = now - s_heard[i].frame.t_us;
            if (quiet_i < 0 || age >= quiet_age) {
                quiet_age = age;
                quiet_i = i;
            }
        }
    }
    if (free_i < 0) {
        free_i = quiet_i < 0 ? 0 : quiet_i;
        memset(&s_heard[free_i], 0, sizeof(s_heard[free_i]));
    }
    s_heard[free_i].used = true;
    s_heard[free_i].pending = true;
    s_heard[free_i].frame = *frame;
    heard_unlock();
}

bool ble_link_up(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void ble_mode_start(void)
{
    if (s_up) {
        return;
    }
    if (s_heard_mu == NULL) {
        s_heard_mu = xSemaphoreCreateMutex();
    }
    if (!s_tx_task) {
        xTaskCreate(tx_task, "ble_tx", 4096, NULL, 6, NULL);
        s_tx_task = true;
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
    if (s_watch_on) {
        ble_npl_callout_stop(&s_watch);
        s_watch_on = false;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
}
