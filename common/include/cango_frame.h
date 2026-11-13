#ifndef CANGO_FRAME_H
#define CANGO_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define CANGO_F_EXT 0x01u
#define CANGO_F_RTR 0x02u
#define CANGO_F_TX  0x04u

#define CANGO_FRAME_BYTES 20

/* Little-endian on the wire and on disk. 20 bytes, no padding. */
typedef struct __attribute__((packed)) {
    uint32_t t_us;
    uint32_t id;
    uint8_t flags;
    uint8_t len;
    uint8_t data[8];
    uint16_t reserved;
} cango_frame_t;

static inline bool cango_frame_ext(const cango_frame_t *f)
{
    return (f->flags & CANGO_F_EXT) != 0;
}

#endif
