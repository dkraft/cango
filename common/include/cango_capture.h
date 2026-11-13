#ifndef CANGO_CAPTURE_H
#define CANGO_CAPTURE_H

#include "cango_frame.h"

#include <stdbool.h>
#include <stdint.h>

#define CANGO_CAP_NONE 0
#define CANGO_CAP_OPEN_STARTUP 1
#define CANGO_CAP_OPEN_DEALER 2
#define CANGO_CAP_OPEN_MANUAL 3
#define CANGO_CAP_WRITE_FRAME 4
#define CANGO_CAP_WRITE_EVENT 5
#define CANGO_CAP_MARKER 6
#define CANGO_CAP_CLOSE_STARTUP 7
#define CANGO_CAP_CLOSE_DEALER 8
#define CANGO_CAP_CLOSE_MANUAL 9
#define CANGO_CAP_COMPLETE 10

#define CANGO_DEST_STARTUP 1
#define CANGO_DEST_DEALER 2

typedef struct {
    uint8_t profile; /* CANGO_PROFILE_* */
    uint32_t startup_max_us;
    uint32_t startup_quiet_us;
    uint32_t dealer_quiet_us;
    uint32_t dealer_context_us;
    uint32_t complete_on_user_events; /* 0 = not a gate */
} cango_capture_cfg_t;

typedef struct cango_capture cango_capture_t;

typedef void (*cango_capture_cb)(void *ctx, int action, const cango_frame_t *frame, uint32_t aux);

cango_capture_t *cango_capture_new(const cango_capture_cfg_t *cfg);
void cango_capture_free(cango_capture_t *c);

void cango_capture_begin(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx);
void cango_capture_on_frame(cango_capture_t *c, const cango_frame_t *frame, cango_capture_cb cb, void *ctx);
void cango_capture_on_user(cango_capture_t *c, uint32_t t_us, cango_capture_cb cb, void *ctx);
void cango_capture_poll(cango_capture_t *c, uint32_t now_us, cango_capture_cb cb, void *ctx);

bool cango_capture_complete(const cango_capture_t *c);
bool cango_capture_running(const cango_capture_t *c);

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t first_us;
    uint32_t last_us;
    uint8_t dlc;
    uint8_t changed_mask;
    uint8_t last[8];
    bool used;
} cango_census_row_t;

#define CANGO_CENSUS_ROWS 64

typedef struct {
    cango_census_row_t rows[CANGO_CENSUS_ROWS];
} cango_census_t;

const cango_census_t *cango_capture_census(const cango_capture_t *c);

void cango_census_add(cango_census_t *c, const cango_frame_t *frame);
/* Writes a JSON object into buf. Returns bytes written, excluding the NUL, or -1. */
int cango_census_json(const cango_census_t *c, char *buf, int cap);

#endif
