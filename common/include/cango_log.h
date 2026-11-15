#ifndef CANGO_LOG_H
#define CANGO_LOG_H

#include "cango_frame.h"

#include <stddef.h>
#include <stdint.h>

#define CANGO_LOG_MAGIC0 'C'
#define CANGO_LOG_MAGIC1 'G'
#define CANGO_LOG_MAGIC2 '0'
#define CANGO_LOG_MAGIC3 '1'
#define CANGO_LOG_VERSION 1
#define CANGO_LOG_HEADER_BYTES 64

#define CANGO_REC_FRAME 1
#define CANGO_REC_GAP 2
#define CANGO_REC_MARKER 3
#define CANGO_REC_USER 4
#define CANGO_REC_ERROR 5

#define CANGO_MARK_STARTUP_BEGIN 1
#define CANGO_MARK_STARTUP_END 2
#define CANGO_MARK_DEALER_BEGIN 3
#define CANGO_MARK_DEALER_END 4
#define CANGO_MARK_PRETRIGGER_EMPTY 5
#define CANGO_MARK_CONTEXT_TRUNC 6

#define CANGO_PROFILE_DEALER 0
#define CANGO_PROFILE_STARTUP 1
#define CANGO_PROFILE_BOTH 2
#define CANGO_PROFILE_MANUAL 3
#define CANGO_PROFILE_WINDOW 4

typedef int (*cango_write_fn)(void *ctx, const void *data, size_t len);

typedef struct cango_log cango_log_t;

typedef struct {
    uint32_t bitrate;
    uint8_t listen_only;
    uint8_t profile;
    char project_id[16];
    uint64_t unix_s;
    uint64_t start_us;
} cango_log_info_t;

cango_log_t *cango_log_open(cango_write_fn write, cango_write_fn flush, void *ctx,
                            const cango_log_info_t *info);
void cango_log_close(cango_log_t *log);
int cango_log_frame(cango_log_t *log, const cango_frame_t *frame);
int cango_log_gap(cango_log_t *log, uint32_t missed);
int cango_log_marker(cango_log_t *log, uint8_t code, uint32_t t_us);
int cango_log_user_event(cango_log_t *log, uint32_t t_us);
int cango_log_bus_error(cango_log_t *log, uint32_t t_us, uint8_t tec, uint8_t rec, uint8_t state);
int cango_log_flush(cango_log_t *log);

/* Reader for host tools and tests. Returns 0, or -1 on a short/bad header. */
typedef struct {
    uint32_t bitrate;
    uint8_t listen_only;
    uint8_t profile;
    char project_id[16];
    uint64_t unix_s;
    uint64_t start_us;
} cango_log_header_t;

int cango_log_parse_header(const uint8_t *buf, size_t len, cango_log_header_t *out);

#endif
