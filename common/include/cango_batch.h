#ifndef CANGO_BATCH_H
#define CANGO_BATCH_H

#include "cango_frame.h"

#include <stddef.h>
#include <stdint.h>

#define CANGO_BATCH_HDR 6
#define CANGO_BATCH_VERSION 1

/* Header: 'C' 'G' ver count seq_le. Then count packed frames. */
size_t cango_batch_size(uint8_t count);
int cango_batch_encode(uint8_t *dst, size_t cap, uint16_t seq, const cango_frame_t *frames, uint8_t count);
int cango_batch_decode(const uint8_t *src, size_t len, uint16_t *seq, cango_frame_t *frames, uint8_t cap, uint8_t *count);

#endif
