#include "cango_filter.h"

#include <string.h>

void cango_filter_clear(cango_filter_t *f)
{
    if (f != NULL) {
        memset(f, 0, sizeof(*f));
    }
}

static cango_filter_slot_t *slot_for(cango_filter_state_t *st, const cango_frame_t *frame)
{
    int i;
    int free_i = -1;
    uint8_t ext = (uint8_t)(frame->flags & CANGO_F_EXT);
    for (i = 0; i < CANGO_FILTER_SLOTS; i++) {
        if (st->slots[i].used && st->slots[i].id == frame->id && st->slots[i].ext == ext) {
            return &st->slots[i];
        }
        if (!st->slots[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = 0;
    }
    memset(&st->slots[free_i], 0, sizeof(st->slots[free_i]));
    st->slots[free_i].used = true;
    st->slots[free_i].id = frame->id;
    st->slots[free_i].ext = ext;
    return &st->slots[free_i];
}

bool cango_filter_match(const cango_filter_t *f, cango_filter_state_t *st, const cango_frame_t *frame)
{
    uint8_t i;
    cango_filter_slot_t *slot;
    bool same;
    if (f == NULL || !f->active || frame == NULL) {
        return true;
    }
    if (f->extended == CANGO_EXT_STD && cango_frame_ext(frame)) {
        return false;
    }
    if (f->extended == CANGO_EXT_EXT && !cango_frame_ext(frame)) {
        return false;
    }
    if ((frame->id & f->mask) != (f->id & f->mask)) {
        return false;
    }
    for (i = 0; i < f->data_n && i < 8; i++) {
        uint8_t idx = f->data[i].index;
        if (idx >= frame->len || idx >= 8) {
            return false;
        }
        if ((frame->data[idx] & f->data[i].mask) != (f->data[i].value & f->data[i].mask)) {
            return false;
        }
    }
    if (st == NULL || (!f->change_only && f->min_interval_ms == 0)) {
        return true;
    }
    slot = slot_for(st, frame);
    same = slot->seen && slot->len == frame->len && memcmp(slot->data, frame->data, 8) == 0;
    slot->seen = true;
    slot->len = frame->len;
    memcpy(slot->data, frame->data, 8);
    if (f->change_only && same) {
        return false;
    }
    if (f->min_interval_ms > 0 && slot->passed) {
        uint32_t dt = frame->t_us - slot->last_pass_us;
        if (dt < (uint32_t)f->min_interval_ms * 1000u) {
            return false;
        }
    }
    slot->passed = true;
    slot->last_pass_us = frame->t_us;
    return true;
}
