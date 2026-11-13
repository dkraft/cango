#include "cango_batch.h"
#include "cango_log.h"

#include <stdio.h>
#include <string.h>

static uint8_t mem[512];
static size_t used;

static int capture(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    if (data == NULL || len == 0) {
        return 0;
    }
    if (used + len > sizeof(mem)) {
        return -1;
    }
    memcpy(mem + used, data, len);
    used += len;
    return 0;
}

static void print_hex(const char *tag, const uint8_t *p, size_t n)
{
    size_t i;
    fputs(tag, stdout);
    for (i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    fputc('\n', stdout);
}

int main(void)
{
    cango_frame_t frame;
    cango_log_info_t info;
    cango_log_t *log;
    uint8_t batch[64];
    int n;

    memset(&frame, 0, sizeof(frame));
    frame.t_us = 1000;
    frame.id = 0x7E0;
    frame.len = 3;
    frame.data[0] = 0x02;
    frame.data[1] = 0x10;
    frame.data[2] = 0x01;
    n = cango_batch_encode(batch, sizeof(batch), 7, &frame, 1);
    if (n < 0) {
        return 1;
    }
    print_hex("BATCH ", batch, (size_t)n);

    memset(&info, 0, sizeof(info));
    info.bitrate = 500000;
    info.listen_only = 1;
    info.profile = CANGO_PROFILE_DEALER;
    memcpy(info.project_id, "generic", 7);
    info.unix_s = 1700000000ull;
    info.start_us = 1000;
    log = cango_log_open(capture, capture, NULL, &info);
    if (log == NULL) {
        return 1;
    }
    if (cango_log_frame(log, &frame) != 0 ||
        cango_log_marker(log, CANGO_MARK_DEALER_BEGIN, 1000) != 0 ||
        cango_log_user_event(log, 1500) != 0 ||
        cango_log_gap(log, 2) != 0) {
        return 1;
    }
    cango_log_close(log);
    print_hex("LOG ", mem, used);
    return 0;
}
