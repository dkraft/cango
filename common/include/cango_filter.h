#ifndef CANGO_FILTER_H
#define CANGO_FILTER_H

#include "cango_frame.h"

#include <stdbool.h>
#include <stdint.h>

#define CANGO_EXT_ANY 0
#define CANGO_EXT_STD 1
#define CANGO_EXT_EXT 2

/* mode 0 keeps the single id/mask. mode 1 hides ids. mode 2 passes ids and an optional range. */
#define CANGO_FILTER_MODE_MASK 0
#define CANGO_FILTER_MODE_ALL 1
#define CANGO_FILTER_MODE_ONLY 2
#define CANGO_FILTER_IDS 32

typedef struct {
    uint8_t index;
    uint8_t value;
    uint8_t mask;
} cango_data_pred_t;

/* Empty means pass every frame. mask 0 with active set still matches all IDs. */
typedef struct {
    bool active;
    uint32_t id;
    uint32_t mask;
    uint8_t extended; /* CANGO_EXT_* */
    uint8_t data_n;
    cango_data_pred_t data[8];
    bool change_only;
    uint16_t min_interval_ms;
    uint8_t mode; /* CANGO_FILTER_MODE_* */
    uint8_t id_n;
    bool has_range;
    uint32_t ids[CANGO_FILTER_IDS];
    uint32_t id_from;
    uint32_t id_to;
} cango_filter_t;

#define CANGO_FILTER_SLOTS 32

typedef struct {
    uint32_t id;
    uint8_t ext;
    uint8_t len;
    uint8_t data[8];
    uint32_t last_pass_us;
    bool seen;
    bool passed;
    bool used;
} cango_filter_slot_t;

typedef struct {
    cango_filter_slot_t slots[CANGO_FILTER_SLOTS];
} cango_filter_state_t;

void cango_filter_clear(cango_filter_t *f);
bool cango_filter_match(const cango_filter_t *f, cango_filter_state_t *st, const cango_frame_t *frame);

#endif
