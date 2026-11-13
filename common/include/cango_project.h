#ifndef CANGO_PROJECT_H
#define CANGO_PROJECT_H

#include "cango_capture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CANGO_PROJECT_ID_LEN 24
#define CANGO_PROJECT_NAME_LEN 32
#define CANGO_TX_ALLOW_MAX 16

typedef struct {
    char id[CANGO_PROJECT_ID_LEN];
    char name[CANGO_PROJECT_NAME_LEN];
    uint32_t bitrate;
    bool listen_only;
    cango_capture_cfg_t logging;
    uint32_t min_free_mb;
    uint8_t tx_allow_n;
    uint32_t tx_allow[CANGO_TX_ALLOW_MAX];
} cango_project_t;

void cango_project_generic(cango_project_t *p);
bool cango_project_is_generic(const char *id);

/* Parses the fields we store. Missing fields keep the Generic defaults.
 * Returns false if the buffer is not an object or the profile is unknown. */
bool cango_project_parse(const char *json, cango_project_t *out);

/* Returns bytes written, excluding NUL, or -1. */
int cango_project_write(const cango_project_t *p, char *buf, int cap);

bool cango_tx_allowed(const cango_project_t *p, uint32_t id);

#endif
