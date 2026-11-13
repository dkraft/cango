#include "cango_log.h"

#include <stdlib.h>
#include <string.h>

struct cango_log {
    cango_write_fn write;
    cango_write_fn flush;
    void *ctx;
};

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static int write_rec(cango_log_t *log, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t hdr[4];
    hdr[0] = type;
    hdr[1] = 0;
    put_u16(hdr + 2, len);
    if (log->write(log->ctx, hdr, 4) != 0) {
        return -1;
    }
    if (len > 0 && log->write(log->ctx, payload, len) != 0) {
        return -1;
    }
    return 0;
}

cango_log_t *cango_log_open(cango_write_fn write, cango_write_fn flush, void *ctx, const cango_log_info_t *info)
{
    cango_log_t *log;
    uint8_t hdr[CANGO_LOG_HEADER_BYTES];
    if (write == NULL || info == NULL) {
        return NULL;
    }
    log = calloc(1, sizeof(*log));
    if (log == NULL) {
        return NULL;
    }
    log->write = write;
    log->flush = flush;
    log->ctx = ctx;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = CANGO_LOG_MAGIC0;
    hdr[1] = CANGO_LOG_MAGIC1;
    hdr[2] = CANGO_LOG_MAGIC2;
    hdr[3] = CANGO_LOG_MAGIC3;
    put_u16(hdr + 4, CANGO_LOG_VERSION);
    put_u16(hdr + 6, CANGO_LOG_HEADER_BYTES);
    put_u32(hdr + 8, info->bitrate);
    hdr[12] = info->listen_only ? 1 : 0;
    hdr[13] = info->profile;
    memcpy(hdr + 16, info->project_id, 16);
    put_u64(hdr + 32, info->unix_s);
    put_u64(hdr + 40, info->start_us);
    if (write(ctx, hdr, sizeof(hdr)) != 0) {
        free(log);
        return NULL;
    }
    return log;
}

void cango_log_close(cango_log_t *log)
{
    if (log == NULL) {
        return;
    }
    cango_log_flush(log);
    free(log);
}

int cango_log_frame(cango_log_t *log, const cango_frame_t *frame)
{
    if (log == NULL || frame == NULL) {
        return -1;
    }
    return write_rec(log, CANGO_REC_FRAME, frame, CANGO_FRAME_BYTES);
}

int cango_log_gap(cango_log_t *log, uint32_t missed)
{
    uint8_t p[4];
    if (log == NULL) {
        return -1;
    }
    put_u32(p, missed);
    return write_rec(log, CANGO_REC_GAP, p, 4);
}

int cango_log_marker(cango_log_t *log, uint8_t code, uint32_t t_us)
{
    uint8_t p[5];
    if (log == NULL) {
        return -1;
    }
    p[0] = code;
    put_u32(p + 1, t_us);
    return write_rec(log, CANGO_REC_MARKER, p, 5);
}

int cango_log_user_event(cango_log_t *log, uint32_t t_us)
{
    uint8_t p[4];
    if (log == NULL) {
        return -1;
    }
    put_u32(p, t_us);
    return write_rec(log, CANGO_REC_USER, p, 4);
}

int cango_log_bus_error(cango_log_t *log, uint32_t t_us, uint8_t tec, uint8_t rec, uint8_t state)
{
    uint8_t p[7];
    if (log == NULL) {
        return -1;
    }
    put_u32(p, t_us);
    p[4] = tec;
    p[5] = rec;
    p[6] = state;
    return write_rec(log, CANGO_REC_ERROR, p, 7);
}

int cango_log_flush(cango_log_t *log)
{
    if (log == NULL) {
        return -1;
    }
    if (log->flush == NULL) {
        return 0;
    }
    return log->flush(log->ctx, NULL, 0);
}

int cango_log_parse_header(const uint8_t *buf, size_t len, cango_log_header_t *out)
{
    if (buf == NULL || out == NULL || len < CANGO_LOG_HEADER_BYTES) {
        return -1;
    }
    if (buf[0] != CANGO_LOG_MAGIC0 || buf[1] != CANGO_LOG_MAGIC1 || buf[2] != CANGO_LOG_MAGIC2 ||
        buf[3] != CANGO_LOG_MAGIC3) {
        return -1;
    }
    if (get_u16(buf + 4) != CANGO_LOG_VERSION || get_u16(buf + 6) != CANGO_LOG_HEADER_BYTES) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->bitrate = get_u32(buf + 8);
    out->listen_only = buf[12];
    out->profile = buf[13];
    memcpy(out->project_id, buf + 16, 16);
    out->unix_s = get_u64(buf + 32);
    out->start_us = get_u64(buf + 40);
    return 0;
}
