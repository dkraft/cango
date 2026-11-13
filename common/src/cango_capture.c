#include "cango_capture.h"

#include "cango_detect.h"
#include "cango_log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define RING_N 512
#define EVENT_N 8

struct cango_capture {
    cango_capture_cfg_t cfg;
    cango_detect_t *det;
    cango_frame_t ring[RING_N];
    uint16_t ring_i;
    uint16_t ring_n;
    bool ring_trunc;
    uint32_t events[EVENT_N];
    uint8_t event_n;
    uint32_t user_events;
    bool begun;
    bool startup_open;
    bool startup_done;
    bool dealer_open;
    bool dealer_done;
    bool complete;
    bool saw_frame;
    uint32_t last_frame_us;
    uint32_t startup_t0;
    cango_census_t census;
};

static bool want_startup(const cango_capture_t *c)
{
    return c->cfg.profile == CANGO_PROFILE_STARTUP || c->cfg.profile == CANGO_PROFILE_BOTH;
}

static bool want_dealer(const cango_capture_t *c)
{
    return c->cfg.profile == CANGO_PROFILE_DEALER || c->cfg.profile == CANGO_PROFILE_BOTH;
}

static void ring_push(cango_capture_t *c, const cango_frame_t *f)
{
    c->ring[c->ring_i] = *f;
    c->ring_i = (uint16_t)((c->ring_i + 1) % RING_N);
    if (c->ring_n < RING_N) {
        c->ring_n++;
    } else {
        c->ring_trunc = true;
    }
}

static void emit(cango_capture_cb cb, void *ctx, int action, const cango_frame_t *frame, uint32_t aux)
{
    if (cb != NULL) {
        cb(ctx, action, frame, aux);
    }
}

static void flush_window(cango_capture_t *c, uint32_t now_us, uint32_t window, uint32_t dest, bool mark_trunc,
                         bool to_census, cango_capture_cb cb, void *ctx)
{
    uint16_t start;
    uint16_t i;
    if (mark_trunc && c->ring_trunc) {
        emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_CONTEXT_TRUNC);
    }
    if (c->ring_n == 0) {
        return;
    }
    start = (uint16_t)((c->ring_i + RING_N - c->ring_n) % RING_N);
    for (i = 0; i < c->ring_n; i++) {
        const cango_frame_t *f = &c->ring[(start + i) % RING_N];
        if (window == 0 || now_us - f->t_us <= window) {
            emit(cb, ctx, CANGO_CAP_WRITE_FRAME, f, dest);
            if (to_census) {
                cango_census_add(&c->census, f);
            }
        }
    }
}

static void flush_events(cango_capture_t *c, cango_capture_cb cb, void *ctx)
{
    uint8_t i;
    for (i = 0; i < c->event_n; i++) {
        emit(cb, ctx, CANGO_CAP_WRITE_EVENT, NULL, c->events[i]);
    }
    c->event_n = 0;
}

static void consider_complete(cango_capture_t *c, cango_capture_cb cb, void *ctx)
{
    bool ok = true;
    if (c->complete) {
        return;
    }
    if (want_startup(c) && !c->startup_done) {
        ok = false;
    }
    if (want_dealer(c) && !c->dealer_done) {
        ok = false;
    }
    if (c->cfg.complete_on_user_events > 0 && c->user_events < c->cfg.complete_on_user_events) {
        ok = false;
    }
    if (ok) {
        c->complete = true;
        emit(cb, ctx, CANGO_CAP_COMPLETE, NULL, 0);
    }
}

static void close_startup(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx)
{
    if (!c->startup_open) {
        return;
    }
    c->startup_open = false;
    c->startup_done = true;
    emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_STARTUP_END);
    emit(cb, ctx, CANGO_CAP_CLOSE_STARTUP, NULL, now_us);
    consider_complete(c, cb, ctx);
}

static void close_dealer(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx)
{
    if (!c->dealer_open) {
        return;
    }
    c->dealer_open = false;
    c->dealer_done = true;
    emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_DEALER_END);
    emit(cb, ctx, CANGO_CAP_CLOSE_DEALER, NULL, now_us);
    consider_complete(c, cb, ctx);
}

cango_capture_t *cango_capture_new(const cango_capture_cfg_t *cfg)
{
    cango_capture_t *c;
    if (cfg == NULL) {
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->cfg = *cfg;
    if (c->cfg.dealer_quiet_us == 0) {
        c->cfg.dealer_quiet_us = 5000000u;
    }
    c->det = cango_detect_new(c->cfg.dealer_quiet_us);
    if (c->det == NULL) {
        free(c);
        return NULL;
    }
    return c;
}

void cango_capture_free(cango_capture_t *c)
{
    if (c == NULL) {
        return;
    }
    cango_detect_free(c->det);
    free(c);
}

void cango_capture_begin(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx)
{
    if (c == NULL || c->begun) {
        return;
    }
    c->begun = true;
    c->startup_t0 = now_us;
    if (want_startup(c)) {
        c->startup_open = true;
        emit(cb, ctx, CANGO_CAP_OPEN_STARTUP, NULL, now_us);
        emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_STARTUP_BEGIN);
        if (c->ring_n == 0) {
            emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_PRETRIGGER_EMPTY);
        } else {
            /* Wake-up pre-trigger is a short window, not the dealer context. */
            flush_window(c, now_us, 50000u, CANGO_DEST_STARTUP, false, true, cb, ctx);
        }
    }
}

void cango_capture_on_frame(cango_capture_t *c, const cango_frame_t *frame, cango_capture_cb cb, void *ctx)
{
    int ev;
    if (c == NULL || frame == NULL || c->complete) {
        return;
    }
    if (!c->begun) {
        ring_push(c, frame);
        return;
    }
    c->saw_frame = true;
    c->last_frame_us = frame->t_us;
    ev = CANGO_DET_NONE;
    if (want_dealer(c) && !c->dealer_done) {
        ev = cango_detect_on_frame(c->det, frame);
    }
    if (ev == CANGO_DET_OPEN) {
        c->dealer_open = true;
        emit(cb, ctx, CANGO_CAP_OPEN_DEALER, NULL, frame->t_us);
        emit(cb, ctx, CANGO_CAP_MARKER, NULL, CANGO_MARK_DEALER_BEGIN);
        flush_events(c, cb, ctx);
        flush_window(c, frame->t_us, c->cfg.dealer_context_us, CANGO_DEST_DEALER, true, false, cb, ctx);
        emit(cb, ctx, CANGO_CAP_WRITE_FRAME, frame, CANGO_DEST_DEALER);
    } else if (c->dealer_open) {
        emit(cb, ctx, CANGO_CAP_WRITE_FRAME, frame, CANGO_DEST_DEALER);
    } else if (want_dealer(c)) {
        ring_push(c, frame);
    }
    if (c->startup_open) {
        emit(cb, ctx, CANGO_CAP_WRITE_FRAME, frame, CANGO_DEST_STARTUP);
        cango_census_add(&c->census, frame);
    }
}

void cango_capture_on_user(cango_capture_t *c, uint32_t t_us, cango_capture_cb cb, void *ctx)
{
    if (c == NULL) {
        return;
    }
    c->user_events++;
    if (c->complete || !c->begun) {
        emit(cb, ctx, CANGO_CAP_OPEN_MANUAL, NULL, t_us);
        emit(cb, ctx, CANGO_CAP_WRITE_EVENT, NULL, t_us);
        emit(cb, ctx, CANGO_CAP_CLOSE_MANUAL, NULL, t_us);
        return;
    }
    if (c->startup_open || c->dealer_open) {
        emit(cb, ctx, CANGO_CAP_WRITE_EVENT, NULL, t_us);
    } else if (c->event_n < EVENT_N) {
        c->events[c->event_n++] = t_us;
    }
    consider_complete(c, cb, ctx);
}

void cango_capture_poll(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx)
{
    int ev;
    if (c == NULL || !c->begun || c->complete) {
        return;
    }
    if (c->startup_open) {
        if (now_us - c->startup_t0 >= c->cfg.startup_max_us && c->cfg.startup_max_us > 0) {
            close_startup(c, now_us, cb, ctx);
        } else if (c->saw_frame && c->cfg.startup_quiet_us > 0 && now_us - c->last_frame_us >= c->cfg.startup_quiet_us) {
            close_startup(c, now_us, cb, ctx);
        }
    }
    if (want_dealer(c) && !c->dealer_done) {
        ev = cango_detect_poll(c->det, now_us);
        if (ev == CANGO_DET_CLOSE) {
            close_dealer(c, now_us, cb, ctx);
        }
    }
}

bool cango_capture_complete(const cango_capture_t *c)
{
    return c != NULL && c->complete;
}

bool cango_capture_running(const cango_capture_t *c)
{
    return c != NULL && c->begun && !c->complete;
}

const cango_census_t *cango_capture_census(const cango_capture_t *c)
{
    return c == NULL ? NULL : &c->census;
}

void cango_census_add(cango_census_t *c, const cango_frame_t *frame)
{
    int i;
    int free_i = -1;
    uint8_t n;
    if (c == NULL || frame == NULL) {
        return;
    }
    for (i = 0; i < CANGO_CENSUS_ROWS; i++) {
        if (c->rows[i].used && c->rows[i].id == frame->id) {
            cango_census_row_t *r = &c->rows[i];
            n = frame->len < 8 ? frame->len : 8;
            if (r->count > 0) {
                uint8_t b;
                uint8_t m = n < r->dlc ? n : r->dlc;
                for (b = 0; b < m; b++) {
                    if (r->last[b] != frame->data[b]) {
                        r->changed_mask = (uint8_t)(r->changed_mask | (1u << b));
                    }
                }
            }
            r->count++;
            r->last_us = frame->t_us;
            r->dlc = frame->len;
            memset(r->last, 0, 8);
            memcpy(r->last, frame->data, n);
            return;
        }
        if (!c->rows[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        return;
    }
    memset(&c->rows[free_i], 0, sizeof(c->rows[free_i]));
    c->rows[free_i].used = true;
    c->rows[free_i].id = frame->id;
    c->rows[free_i].count = 1;
    c->rows[free_i].first_us = frame->t_us;
    c->rows[free_i].last_us = frame->t_us;
    c->rows[free_i].dlc = frame->len;
    n = frame->len < 8 ? frame->len : 8;
    memcpy(c->rows[free_i].last, frame->data, n);
}

int cango_census_json(const cango_census_t *c, char *buf, int cap)
{
    int used = 0;
    int i;
    int n = 0;
    if (c == NULL || buf == NULL || cap < 3) {
        return -1;
    }
    used = snprintf(buf, (size_t)cap, "{\"ids\":[");
    if (used < 0 || used >= cap) {
        return -1;
    }
    for (i = 0; i < CANGO_CENSUS_ROWS; i++) {
        int w;
        const cango_census_row_t *r = &c->rows[i];
        if (!r->used) {
            continue;
        }
        w = snprintf(buf + used, (size_t)(cap - used),
                     "%s{\"id\":%" PRIu32 ",\"count\":%" PRIu32 ",\"first_us\":%" PRIu32
                     ",\"last_us\":%" PRIu32 ",\"dlc\":%u,\"changed_mask\":%u}",
                     n ? "," : "", r->id, r->count, r->first_us, r->last_us, r->dlc, r->changed_mask);
        if (w < 0 || used + w >= cap) {
            return -1;
        }
        used += w;
        n++;
    }
    if (used + 2 >= cap) {
        return -1;
    }
    buf[used++] = ']';
    buf[used++] = '}';
    buf[used] = 0;
    return used;
}
