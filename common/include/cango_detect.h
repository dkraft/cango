#ifndef CANGO_DETECT_H
#define CANGO_DETECT_H

#include "cango_frame.h"

#include <stdbool.h>
#include <stdint.h>

#define CANGO_DET_NONE 0
#define CANGO_DET_OPEN 1
#define CANGO_DET_DIAG 2
#define CANGO_DET_CLOSE 3

typedef struct cango_detect cango_detect_t;

cango_detect_t *cango_detect_new(uint32_t quiet_us);
void cango_detect_free(cango_detect_t *d);
void cango_detect_reset(cango_detect_t *d);

/* Returns CANGO_DET_OPEN, CANGO_DET_DIAG, or CANGO_DET_NONE.
 * OPEN means this frame opened the session and is itself diagnostic. */
int cango_detect_on_frame(cango_detect_t *d, const cango_frame_t *frame);

/* Call with the current time so a quiet session can close. */
int cango_detect_poll(cango_detect_t *d, uint32_t now_us);

bool cango_detect_session(const cango_detect_t *d);

#endif
