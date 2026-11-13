#include "cango_detect.h"

#include <stdlib.h>
#include <string.h>

#define ID_SLOTS 48
#define REQ_SLOTS 8
#define FAST_PERIOD_US 200000u
#define PAIR_WINDOW_US 250000u
#define STEADY_NEED 4

typedef struct {
    uint32_t id;
    uint8_t ext;
    uint32_t count;
    uint32_t last_us;
    uint32_t period_us;
    uint8_t steady;
    bool periodic;
    bool used;
} id_slot_t;

typedef struct {
    uint32_t id;
    uint8_t ext;
    uint32_t t_us;
    bool used;
} req_slot_t;

struct cango_detect {
    id_slot_t ids[ID_SLOTS];
    req_slot_t reqs[REQ_SLOTS];
    bool session;
    uint32_t last_diag_us;
    uint32_t quiet_us;
    uint8_t req_next;
};

static bool request_id(uint32_t id, bool ext)
{
    if (ext) {
        return false;
    }
    if (id == 0x7DFu) {
        return true;
    }
    return id >= 0x7E0u && id <= 0x7E7u;
}

static int pci_kind(const cango_frame_t *f)
{
    uint8_t t;
    uint8_t n;
    if (f->len < 1) {
        return -1;
    }
    t = (uint8_t)(f->data[0] >> 4);
    if (t > 3) {
        return -1;
    }
    if (t == 0) {
        n = (uint8_t)(f->data[0] & 0x0f);
        if (n > 7 || f->len < (uint8_t)(n + 1)) {
            return -1;
        }
    }
    return (int)t;
}

static id_slot_t *id_slot(cango_detect_t *d, const cango_frame_t *f)
{
    int i;
    int free_i = -1;
    uint8_t ext = cango_frame_ext(f) ? 1 : 0;
    for (i = 0; i < ID_SLOTS; i++) {
        if (d->ids[i].used && d->ids[i].id == f->id && d->ids[i].ext == ext) {
            return &d->ids[i];
        }
        if (!d->ids[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = 0;
        memset(&d->ids[0], 0, sizeof(d->ids[0]));
    }
    memset(&d->ids[free_i], 0, sizeof(d->ids[free_i]));
    d->ids[free_i].used = true;
    d->ids[free_i].id = f->id;
    d->ids[free_i].ext = ext;
    return &d->ids[free_i];
}

static void note_period(id_slot_t *s, uint32_t t_us)
{
    uint32_t dt;
    if (s->count == 0) {
        s->last_us = t_us;
        s->count = 1;
        return;
    }
    dt = t_us - s->last_us;
    s->last_us = t_us;
    s->count++;
    if (dt == 0) {
        return;
    }
    if (s->period_us == 0) {
        s->period_us = dt;
        s->steady = 1;
        return;
    }
    /* Within 20% of the running period. */
    if (dt + (s->period_us / 5) >= s->period_us && dt <= s->period_us + (s->period_us / 5)) {
        if (s->steady < 255) {
            s->steady++;
        }
        s->period_us = (s->period_us * 3 + dt) / 4;
        if (s->steady >= STEADY_NEED) {
            s->periodic = true;
        }
    } else {
        s->steady = 0;
        s->periodic = false;
        s->period_us = dt;
    }
}

static void remember_req(cango_detect_t *d, const cango_frame_t *f)
{
    req_slot_t *r = &d->reqs[d->req_next % REQ_SLOTS];
    d->req_next++;
    r->used = true;
    r->id = f->id;
    r->ext = cango_frame_ext(f) ? 1 : 0;
    r->t_us = f->t_us;
}

static bool paired(cango_detect_t *d, const cango_frame_t *f)
{
    int i;
    bool ext = cango_frame_ext(f);
    for (i = 0; i < REQ_SLOTS; i++) {
        req_slot_t *r = &d->reqs[i];
        uint32_t dt;
        if (!r->used) {
            continue;
        }
        dt = f->t_us - r->t_us;
        if (dt > PAIR_WINDOW_US) {
            continue;
        }
        if (!ext && !r->ext && f->id == r->id + 8u) {
            return true;
        }
        if (ext && r->ext && f->id != r->id) {
            return true;
        }
    }
    return false;
}

cango_detect_t *cango_detect_new(uint32_t quiet_us)
{
    cango_detect_t *d = calloc(1, sizeof(*d));
    if (d != NULL) {
        d->quiet_us = quiet_us == 0 ? 5000000u : quiet_us;
    }
    return d;
}

void cango_detect_free(cango_detect_t *d)
{
    free(d);
}

void cango_detect_reset(cango_detect_t *d)
{
    uint32_t quiet;
    if (d == NULL) {
        return;
    }
    quiet = d->quiet_us;
    memset(d, 0, sizeof(*d));
    d->quiet_us = quiet;
}

int cango_detect_on_frame(cango_detect_t *d, const cango_frame_t *frame)
{
    id_slot_t *s;
    int pci;
    bool ext;
    bool fast;
    bool diag = false;
    if (d == NULL || frame == NULL) {
        return CANGO_DET_NONE;
    }
    s = id_slot(d, frame);
    note_period(s, frame->t_us);
    pci = pci_kind(frame);
    ext = cango_frame_ext(frame);
    fast = s->periodic && s->period_us < FAST_PERIOD_US && s->period_us != 0;
    if (!fast && pci >= 0) {
        if (request_id(frame->id, ext) && (pci == 0 || pci == 1)) {
            diag = true;
            remember_req(d, frame);
        } else if (!s->periodic && (pci == 0 || pci == 1) && paired(d, frame)) {
            diag = true;
        } else if (!s->periodic && (pci == 0 || pci == 1)) {
            remember_req(d, frame);
        }
    }
    if (!diag) {
        return CANGO_DET_NONE;
    }
    d->last_diag_us = frame->t_us;
    if (!d->session) {
        d->session = true;
        return CANGO_DET_OPEN;
    }
    return CANGO_DET_DIAG;
}

int cango_detect_poll(cango_detect_t *d, uint32_t now_us)
{
    if (d == NULL || !d->session) {
        return CANGO_DET_NONE;
    }
    if (now_us - d->last_diag_us >= d->quiet_us) {
        d->session = false;
        return CANGO_DET_CLOSE;
    }
    return CANGO_DET_NONE;
}

bool cango_detect_session(const cango_detect_t *d)
{
    return d != NULL && d->session;
}
