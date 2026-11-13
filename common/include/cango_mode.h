#ifndef CANGO_MODE_H
#define CANGO_MODE_H

#include <stdbool.h>
#include <stdint.h>

#define CANGO_MODE_CAPTURE 0
#define CANGO_MODE_BLE 1
#define CANGO_MODE_WIFI 2

#define CANGO_BTN_NONE 0
#define CANGO_BTN_SHORT 1
#define CANGO_BTN_LONG 2

#define CANGO_SHORT_MAX_MS 600u
#define CANGO_LONG_MS 800u
#define CANGO_DEBOUNCE_MS 40u

typedef struct {
    bool in_boot;
    bool down;
    uint32_t down_at_ms;
    bool long_fired;
} cango_button_t;

typedef struct {
    bool stable;
    bool last_raw;
    uint32_t since_ms;
    cango_button_t button;
} cango_debounce_t;

uint8_t cango_mode_next(uint8_t mode);

void cango_button_init(cango_button_t *b);
/* Drop boot lock. A button already held is swallowed until release. */
void cango_button_end_boot(cango_button_t *b, bool currently_down);
/* pressed is the debounced level, true when the button is down. */
int cango_button_feed(cango_button_t *b, bool pressed, uint32_t now_ms);

void cango_debounce_init(cango_debounce_t *d);
void cango_debounce_end_boot(cango_debounce_t *d);
int cango_debounce_feed(cango_debounce_t *d, bool raw_down, uint32_t now_ms);

#endif
