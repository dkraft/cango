#ifndef CANGO_GVRET_H
#define CANGO_GVRET_H

#include "cango_frame.h"

#include <stdbool.h>
#include <stdint.h>

/* SavvyCAN GVRET binary. Two 0xE7 bytes, then commands that start with 0xF1.
 * Command 0x05 is consumed and not answered: SavvyCAN treats it as having no reply.
 * A reply there would desync the parser. The bitrate stays whatever the project set.
 */

#define CANGO_GVRET_PORT 23

typedef struct {
    uint8_t st;
    uint8_t cmd;
    uint8_t phase;
    uint8_t need;
    uint8_t acc[16];
    uint8_t acc_n;
    int tx_calls;
    int tx_blocked;
} cango_gvret_link_t;

typedef bool (*cango_gvret_allow_fn)(uint32_t id, void *ctx);
typedef bool (*cango_gvret_tx_fn)(const cango_frame_t *frame, void *ctx);

void cango_gvret_init(cango_gvret_link_t *link);

/* Feed host bytes. Replies are appended to out. Returns the reply length, or -1
 * if out was too small (bytes already consumed still advance the parser).
 * tx is called only when armed, the bus is 0, and allow returns true.
 */
int cango_gvret_feed(cango_gvret_link_t *link, const uint8_t *in, int n, uint32_t now_us, uint32_t bitrate,
                     bool listen_only, bool armed, cango_gvret_allow_fn allow, void *allow_ctx, cango_gvret_tx_fn tx,
                     void *tx_ctx, uint8_t *out, int cap);

/* Device to SavvyCAN frame. Includes the trailing 0x00 SavvyCAN ignores on a data frame. */
int cango_gvret_encode_frame(uint8_t *out, int cap, const cango_frame_t *frame);

#endif
