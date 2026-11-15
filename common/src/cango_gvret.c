#include "cango_gvret.h"

#include <string.h>

#define ST_IDLE 0
#define ST_E7 1
#define ST_CMD 2
#define ST_BODY 3

void cango_gvret_init(cango_gvret_link_t *link)
{
    if (link != NULL) {
        memset(link, 0, sizeof(*link));
    }
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int cango_gvret_encode_frame(uint8_t *out, int cap, const cango_frame_t *frame)
{
    uint8_t len;
    uint32_t id;
    int n;
    if (out == NULL || frame == NULL) {
        return -1;
    }
    len = frame->len > 8 ? 8 : frame->len;
    n = 2 + 4 + 4 + 1 + (int)len + 1;
    if (cap < n) {
        return -1;
    }
    out[0] = 0xF1;
    out[1] = 0x00;
    put_u32(out + 2, frame->t_us);
    id = frame->id & 0x1fffffffu;
    if ((frame->flags & CANGO_F_EXT) != 0) {
        id |= 0x80000000u;
    }
    put_u32(out + 6, id);
    out[10] = (uint8_t)(len & 0x0f);
    if (len > 0) {
        memcpy(out + 11, frame->data, len);
    }
    out[11 + len] = 0x00;
    return n;
}

typedef struct {
    uint8_t *out;
    int cap;
    int n;
    bool overflow;
} wr_t;

static void putb(wr_t *w, uint8_t b)
{
    if (w->n < w->cap && w->out != NULL) {
        w->out[w->n] = b;
    } else {
        w->overflow = true;
    }
    w->n++;
}

static void reply_buses(wr_t *w, uint32_t bitrate, bool listen_only)
{
    uint8_t speed[4];
    int i;
    put_u32(speed, bitrate);
    putb(w, 0xF1);
    putb(w, 0x06);
    putb(w, listen_only ? 0x11 : 0x01);
    for (i = 0; i < 4; i++) {
        putb(w, speed[i]);
    }
    for (i = 0; i < 5; i++) {
        putb(w, 0);
    }
}

static void reply_info(wr_t *w)
{
    int i;
    putb(w, 0xF1);
    putb(w, 0x07);
    putb(w, 0x01);
    putb(w, 0x00);
    for (i = 0; i < 4; i++) {
        putb(w, 0);
    }
}

static void reply_time(wr_t *w, uint32_t now_us)
{
    uint8_t t[4];
    int i;
    put_u32(t, now_us);
    putb(w, 0xF1);
    putb(w, 0x01);
    for (i = 0; i < 4; i++) {
        putb(w, t[i]);
    }
}

static void reply_buses_n(wr_t *w)
{
    putb(w, 0xF1);
    putb(w, 0x0C);
    putb(w, 0x01);
}

static void reply_ext(wr_t *w)
{
    int i;
    putb(w, 0xF1);
    putb(w, 0x0D);
    for (i = 0; i < 15; i++) {
        putb(w, 0);
    }
}

static void reply_alive(wr_t *w)
{
    putb(w, 0xF1);
    putb(w, 0x09);
}

static void take_tx(cango_gvret_link_t *link, bool armed, cango_gvret_allow_fn allow, void *allow_ctx, cango_gvret_tx_fn tx,
                    void *tx_ctx)
{
    uint32_t id;
    uint8_t bus;
    uint8_t len;
    cango_frame_t frame;
    id = get_u32(link->acc);
    bus = (uint8_t)(link->acc[4] & 0x03);
    len = link->acc[5];
    if (len > 8) {
        link->tx_blocked++;
        return;
    }
    if (!armed || bus != 0 || allow == NULL || !allow(id & 0x1fffffffu, allow_ctx)) {
        link->tx_blocked++;
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.id = id & 0x1fffffffu;
    frame.len = len;
    frame.flags = CANGO_F_TX;
    if ((id & 0x80000000u) != 0) {
        frame.flags = (uint8_t)(frame.flags | CANGO_F_EXT);
    }
    if (len > 0) {
        memcpy(frame.data, link->acc + 6, len);
    }
    link->tx_calls++;
    if (tx != NULL) {
        tx(&frame, tx_ctx);
    }
}

static void begin_body(cango_gvret_link_t *link, uint8_t cmd)
{
    link->cmd = cmd;
    link->st = ST_BODY;
    link->acc_n = 0;
    link->phase = 0;
    if (cmd == 0x00) {
        link->need = 6;
    } else if (cmd == 0x05) {
        link->need = 9;
        link->phase = 1;
    } else if (cmd == 0x0E) {
        link->need = 13;
        link->phase = 1;
    } else {
        link->st = ST_IDLE;
    }
}

static void on_cmd(cango_gvret_link_t *link, uint8_t cmd, wr_t *w, uint32_t now_us, uint32_t bitrate, bool listen_only)
{
    if (cmd == 0x01) {
        reply_time(w, now_us);
        link->st = ST_IDLE;
    } else if (cmd == 0x06) {
        reply_buses(w, bitrate, listen_only);
        link->st = ST_IDLE;
    } else if (cmd == 0x07) {
        reply_info(w);
        link->st = ST_IDLE;
    } else if (cmd == 0x09) {
        reply_alive(w);
        link->st = ST_IDLE;
    } else if (cmd == 0x0C) {
        reply_buses_n(w);
        link->st = ST_IDLE;
    } else if (cmd == 0x0D) {
        reply_ext(w);
        link->st = ST_IDLE;
    } else if (cmd == 0x00 || cmd == 0x05 || cmd == 0x0E) {
        begin_body(link, cmd);
    } else {
        link->st = ST_IDLE;
    }
}

static void on_body_byte(cango_gvret_link_t *link, uint8_t b, bool armed, cango_gvret_allow_fn allow, void *allow_ctx,
                         cango_gvret_tx_fn tx, void *tx_ctx)
{
    if (link->acc_n < sizeof(link->acc)) {
        link->acc[link->acc_n] = b;
    }
    if (link->acc_n < 255) {
        link->acc_n++;
    }
    if (link->need > 0) {
        link->need--;
    }
    if (link->need > 0) {
        return;
    }
    if (link->cmd == 0x00 && link->phase == 0) {
        uint8_t len = link->acc[5];
        if (len > 8 || link->acc_n > sizeof(link->acc)) {
            link->tx_blocked++;
            link->st = ST_IDLE;
            return;
        }
        link->phase = 1;
        link->need = (uint8_t)(len + 1);
        if (link->need > 0) {
            return;
        }
    }
    if (link->cmd == 0x00) {
        take_tx(link, armed, allow, allow_ctx, tx, tx_ctx);
    }
    link->st = ST_IDLE;
}

int cango_gvret_feed(cango_gvret_link_t *link, const uint8_t *in, int n, uint32_t now_us, uint32_t bitrate, bool listen_only,
                     bool armed, cango_gvret_allow_fn allow, void *allow_ctx, cango_gvret_tx_fn tx, void *tx_ctx, uint8_t *out,
                     int cap)
{
    wr_t w;
    int i;
    if (link == NULL) {
        return -1;
    }
    memset(&w, 0, sizeof(w));
    w.out = out;
    w.cap = cap > 0 ? cap : 0;
    if (in == NULL || n <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint8_t b = in[i];
        if (link->st == ST_BODY) {
            on_body_byte(link, b, armed, allow, allow_ctx, tx, tx_ctx);
            continue;
        }
        if (link->st == ST_E7) {
            link->st = (b == 0xF1) ? ST_CMD : ST_IDLE;
            continue;
        }
        if (link->st == ST_CMD) {
            on_cmd(link, b, &w, now_us, bitrate, listen_only);
            continue;
        }
        if (b == 0xE7) {
            link->st = ST_E7;
        } else if (b == 0xF1) {
            link->st = ST_CMD;
        }
    }
    if (w.overflow) {
        return -1;
    }
    return w.n;
}
