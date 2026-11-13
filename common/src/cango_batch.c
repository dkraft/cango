#include "cango_batch.h"

#include <string.h>

size_t cango_batch_size(uint8_t count)
{
    return (size_t)CANGO_BATCH_HDR + (size_t)count * CANGO_FRAME_BYTES;
}

int cango_batch_encode(uint8_t *dst, size_t cap, uint16_t seq, const cango_frame_t *frames, uint8_t count)
{
    size_t need;
    if (dst == NULL || (count > 0 && frames == NULL)) {
        return -1;
    }
    need = cango_batch_size(count);
    if (cap < need) {
        return -1;
    }
    dst[0] = 'C';
    dst[1] = 'G';
    dst[2] = CANGO_BATCH_VERSION;
    dst[3] = count;
    dst[4] = (uint8_t)(seq & 0xff);
    dst[5] = (uint8_t)((seq >> 8) & 0xff);
    if (count > 0) {
        memcpy(dst + CANGO_BATCH_HDR, frames, (size_t)count * CANGO_FRAME_BYTES);
    }
    return (int)need;
}

int cango_batch_decode(const uint8_t *src, size_t len, uint16_t *seq, cango_frame_t *frames, uint8_t cap, uint8_t *count)
{
    uint8_t n;
    if (src == NULL || seq == NULL || count == NULL || len < CANGO_BATCH_HDR) {
        return -1;
    }
    if (src[0] != 'C' || src[1] != 'G' || src[2] != CANGO_BATCH_VERSION) {
        return -1;
    }
    n = src[3];
    if (len < cango_batch_size(n) || n > cap || (n > 0 && frames == NULL)) {
        return -1;
    }
    *seq = (uint16_t)(src[4] | ((uint16_t)src[5] << 8));
    *count = n;
    if (n > 0) {
        memcpy(frames, src + CANGO_BATCH_HDR, (size_t)n * CANGO_FRAME_BYTES);
    }
    return 0;
}
