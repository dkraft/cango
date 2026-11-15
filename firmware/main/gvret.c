#include "app.h"
#include "cango_gvret.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

static const char *TAG = "gvret";

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static volatile bool s_run;
static volatile int s_listen = -1;
static volatile int s_client = -1;
static cango_gvret_link_t s_link;
static uint8_t s_tx[1024];
static int s_tx_len;

static bool gvret_allow(uint32_t id, void *ctx)
{
    bool ok;
    (void)ctx;
    app_lock();
    ok = cango_tx_allowed(&g_app.project, id);
    app_unlock();
    return ok;
}

static bool gvret_tx(const cango_frame_t *frame, void *ctx)
{
    (void)ctx;
    return can_request_send(frame);
}

static void count_drop(void)
{
    app_lock();
    g_app.dropped++;
    app_unlock();
}

/* True when the socket is still usable. False means the peer is gone. */
static bool tx_flush(int fd)
{
    while (s_tx_len > 0) {
        int w = send(fd, s_tx, (size_t)s_tx_len, MSG_DONTWAIT);
        if (w < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if (w == 0) {
            return false;
        }
        if (w < s_tx_len) {
            memmove(s_tx, s_tx + w, (size_t)(s_tx_len - w));
        }
        s_tx_len -= w;
    }
    return true;
}

static bool tx_queue(int fd, const uint8_t *data, int n)
{
    if (n <= 0) {
        return true;
    }
    if (!tx_flush(fd)) {
        return false;
    }
    if (s_tx_len + n > (int)sizeof(s_tx)) {
        return true;
    }
    memcpy(s_tx + s_tx_len, data, (size_t)n);
    s_tx_len += n;
    return tx_flush(fd);
}

static void close_client(void)
{
    int fd = s_client;
    s_client = -1;
    s_tx_len = 0;
    if (fd >= 0) {
        close(fd);
    }
    if (s_q != NULL) {
        xQueueReset(s_q);
    }
}

static void accept_one(int listen_fd)
{
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    int fd = accept(listen_fd, (struct sockaddr *)&src, &sl);
    int flags;
    if (fd < 0) {
        return;
    }
    close_client();
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    cango_gvret_init(&s_link);
    s_client = fd;
    ESP_LOGI(TAG, "client");
}

static void read_client(int fd)
{
    uint8_t buf[256];
    uint8_t reply[160];
    int n;
    int out_n;
    bool armed;
    n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        ESP_LOGI(TAG, "client closed");
        close_client();
        return;
    }
    if (n < 0) {
        return;
    }
    armed = g_app.armed && app_now_us() <= g_app.arm_until_us;
    out_n = cango_gvret_feed(&s_link, buf, n, app_now_us(), g_app.bitrate, !armed, armed, gvret_allow, NULL, gvret_tx, NULL,
                             reply, (int)sizeof(reply));
    if (out_n > 0 && s_client == fd && !tx_queue(fd, reply, out_n)) {
        close_client();
    }
}

static void drain_frames(int fd)
{
    if (!tx_flush(fd)) {
        close_client();
        return;
    }
    while (s_client == fd && s_tx_len + 32 <= (int)sizeof(s_tx)) {
        cango_frame_t frame;
        uint8_t raw[32];
        int n;
        if (xQueueReceive(s_q, &frame, 0) != pdTRUE) {
            return;
        }
        n = cango_gvret_encode_frame(raw, (int)sizeof(raw), &frame);
        if (n < 0) {
            continue;
        }
        if (!tx_queue(fd, raw, n)) {
            close_client();
            return;
        }
    }
}

static void gvret_task(void *arg)
{
    struct sockaddr_in addr;
    int opt = 1;
    int ls;
    (void)arg;
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) {
        ESP_LOGE(TAG, "socket");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CANGO_GVRET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) {
        ESP_LOGE(TAG, "bind %d", CANGO_GVRET_PORT);
        close(ls);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    fcntl(ls, F_SETFL, O_NONBLOCK);
    s_listen = ls;
    ESP_LOGI(TAG, "tcp %d", CANGO_GVRET_PORT);
    while (s_run) {
        fd_set rfds;
        struct timeval tv;
        int fd = s_client;
        int nfds = ls;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        if (fd >= 0) {
            FD_SET(fd, &rfds);
            if (fd > nfds) {
                nfds = fd;
            }
        }
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        if (select(nfds + 1, &rfds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(ls, &rfds)) {
                accept_one(ls);
            }
            fd = s_client;
            if (fd >= 0 && FD_ISSET(fd, &rfds)) {
                read_client(fd);
            }
        }
        fd = s_client;
        if (fd >= 0) {
            drain_frames(fd);
        }
    }
    close_client();
    if (s_listen >= 0) {
        close(s_listen);
        s_listen = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void gvret_offer_frame(const cango_frame_t *frame)
{
    if (!s_run || s_client < 0 || s_q == NULL || frame == NULL) {
        return;
    }
    if (xQueueSend(s_q, frame, 0) != pdTRUE) {
        count_drop();
    }
}

void gvret_start(void)
{
    int i;
    for (i = 0; i < 30 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_q == NULL) {
        s_q = xQueueCreate(64, sizeof(cango_frame_t));
    }
    s_run = true;
    if (s_task == NULL) {
        xTaskCreate(gvret_task, "gvret", 4096, NULL, 5, &s_task);
    }
}

void gvret_stop(void)
{
    s_run = false;
}
