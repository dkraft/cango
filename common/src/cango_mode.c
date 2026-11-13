#include "cango_mode.h"

#include <stddef.h>

uint8_t cango_mode_next(uint8_t mode)
{
    if (mode == CANGO_MODE_CAPTURE) {
        return CANGO_MODE_BLE;
    }
    if (mode == CANGO_MODE_BLE) {
        return CANGO_MODE_WIFI;
    }
    return CANGO_MODE_CAPTURE;
}

void cango_button_init(cango_button_t *b)
{
    if (b == NULL) {
        return;
    }
    b->in_boot = true;
    b->down = false;
    b->down_at_ms = 0;
    b->long_fired = false;
}

void cango_button_end_boot(cango_button_t *b, bool currently_down)
{
    if (b == NULL) {
        return;
    }
    b->in_boot = false;
    b->down = currently_down;
    b->long_fired = currently_down;
}

int cango_button_feed(cango_button_t *b, bool pressed, uint32_t now_ms)
{
    if (b == NULL) {
        return CANGO_BTN_NONE;
    }
    if (b->in_boot) {
        b->down = false;
        b->long_fired = false;
        return CANGO_BTN_NONE;
    }
    if (pressed && !b->down) {
        b->down = true;
        b->down_at_ms = now_ms;
        b->long_fired = false;
        return CANGO_BTN_NONE;
    }
    if (pressed && b->down && !b->long_fired) {
        if (now_ms - b->down_at_ms >= CANGO_LONG_MS) {
            b->long_fired = true;
            return CANGO_BTN_LONG;
        }
        return CANGO_BTN_NONE;
    }
    if (!pressed && b->down) {
        uint32_t held = now_ms - b->down_at_ms;
        b->down = false;
        if (!b->long_fired && held < CANGO_SHORT_MAX_MS) {
            return CANGO_BTN_SHORT;
        }
    }
    return CANGO_BTN_NONE;
}

void cango_debounce_init(cango_debounce_t *d)
{
    if (d == NULL) {
        return;
    }
    d->stable = false;
    d->last_raw = false;
    d->since_ms = 0;
    cango_button_init(&d->button);
}

void cango_debounce_end_boot(cango_debounce_t *d)
{
    if (d == NULL) {
        return;
    }
    cango_button_end_boot(&d->button, d->stable);
}

int cango_debounce_feed(cango_debounce_t *d, bool raw_down, uint32_t now_ms)
{
    if (d == NULL) {
        return CANGO_BTN_NONE;
    }
    if (raw_down != d->last_raw) {
        d->last_raw = raw_down;
        d->since_ms = now_ms;
        return CANGO_BTN_NONE;
    }
    if (now_ms - d->since_ms >= CANGO_DEBOUNCE_MS && d->stable != raw_down) {
        d->stable = raw_down;
    }
    return cango_button_feed(&d->button, d->stable, now_ms);
}
