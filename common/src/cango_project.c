#include "cango_project.h"

#include "cango_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') {
        s++;
    }
    return s;
}

static const char *find_key(const char *json, const char *key)
{
    char pat[64];
    const char *p;
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(pat)) {
        return NULL;
    }
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *after = skip_ws(p + n);
        if (*after == ':') {
            return skip_ws(after + 1);
        }
        p += n;
    }
    return NULL;
}

static bool parse_string(const char *p, char *out, size_t cap)
{
    size_t i = 0;
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    while (*p && *p != '"') {
        if (i + 1 >= cap) {
            return false;
        }
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    if (*p != '"') {
        return false;
    }
    out[i] = 0;
    return true;
}

static bool parse_u32(const char *p, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (p == NULL) {
        return false;
    }
    v = strtoul(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

void cango_project_generic(cango_project_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    memcpy(p->id, "generic", 8);
    memcpy(p->name, "Generic", 8);
    p->bitrate = 500000;
    p->listen_only = true;
    p->logging.profile = CANGO_PROFILE_DEALER;
    p->logging.startup_max_us = 60u * 1000000u;
    p->logging.startup_quiet_us = 3u * 1000000u;
    p->logging.dealer_quiet_us = 5u * 1000000u;
    p->logging.dealer_context_us = 5u * 1000000u;
    p->logging.complete_on_user_events = 0;
    p->min_free_mb = 64;
}

bool cango_project_is_generic(const char *id)
{
    return id != NULL && strcmp(id, "generic") == 0;
}

static uint8_t profile_from_name(const char *s)
{
    if (strcmp(s, "startup") == 0) {
        return CANGO_PROFILE_STARTUP;
    }
    if (strcmp(s, "both") == 0) {
        return CANGO_PROFILE_BOTH;
    }
    if (strcmp(s, "dealer") == 0) {
        return CANGO_PROFILE_DEALER;
    }
    return 0xff;
}

static const char *profile_name(uint8_t profile)
{
    if (profile == CANGO_PROFILE_STARTUP) {
        return "startup";
    }
    if (profile == CANGO_PROFILE_BOTH) {
        return "both";
    }
    return "dealer";
}

bool cango_project_parse(const char *json, cango_project_t *out)
{
    const char *p;
    char tmp[32];
    uint32_t n;
    uint8_t prof;
    if (json == NULL || out == NULL) {
        return false;
    }
    cango_project_generic(out);
    if (skip_ws(json)[0] != '{') {
        return false;
    }
    p = find_key(json, "id");
    if (p != NULL) {
        if (!parse_string(p, out->id, sizeof(out->id))) {
            return false;
        }
    }
    p = find_key(json, "name");
    if (p != NULL && !parse_string(p, out->name, sizeof(out->name))) {
        return false;
    }
    p = find_key(json, "bitrate");
    if (p != NULL && parse_u32(p, &n) && n >= 10000 && n <= 1000000) {
        out->bitrate = n;
    }
    p = find_key(json, "listen_only");
    if (p != NULL) {
        if (strncmp(p, "false", 5) == 0) {
            out->listen_only = false;
        } else if (strncmp(p, "true", 4) == 0) {
            out->listen_only = true;
        }
    }
    p = find_key(json, "capture_profile");
    if (p != NULL) {
        if (!parse_string(p, tmp, sizeof(tmp))) {
            return false;
        }
        prof = profile_from_name(tmp);
        if (prof == 0xff) {
            return false;
        }
        out->logging.profile = prof;
    }
    p = find_key(json, "startup_max_s");
    if (p != NULL && parse_u32(p, &n)) {
        out->logging.startup_max_us = n * 1000000u;
    }
    p = find_key(json, "startup_quiet_s");
    if (p != NULL && parse_u32(p, &n)) {
        out->logging.startup_quiet_us = n * 1000000u;
    }
    p = find_key(json, "dealer_quiet_s");
    if (p != NULL && parse_u32(p, &n)) {
        out->logging.dealer_quiet_us = n * 1000000u;
    }
    p = find_key(json, "dealer_context_s");
    if (p != NULL && parse_u32(p, &n)) {
        out->logging.dealer_context_us = n * 1000000u;
    }
    p = find_key(json, "complete_on_user_events");
    if (p != NULL && parse_u32(p, &n)) {
        out->logging.complete_on_user_events = n;
    }
    p = find_key(json, "min_free_mb");
    if (p != NULL && parse_u32(p, &n)) {
        out->min_free_mb = n;
    }
    p = find_key(json, "tx_allow");
    if (p != NULL && *p == '[') {
        p++;
        out->tx_allow_n = 0;
        while (*p && *p != ']' && out->tx_allow_n < CANGO_TX_ALLOW_MAX) {
            p = skip_ws(p);
            if (*p == ']') {
                break;
            }
            if (!parse_u32(p, &n)) {
                break;
            }
            out->tx_allow[out->tx_allow_n++] = n;
            while (*p && *p != ',' && *p != ']') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }
    }
    return true;
}

int cango_project_write(const cango_project_t *p, char *buf, int cap)
{
    int n;
    int i;
    if (p == NULL || buf == NULL) {
        return -1;
    }
    n = snprintf(buf, (size_t)cap,
                 "{\"id\":\"%s\",\"name\":\"%s\",\"bitrate\":%" PRIu32 ",\"listen_only\":%s,"
                 "\"saved_filters\":[],\"logging\":{\"capture_profile\":\"%s\","
                 "\"startup_max_s\":%" PRIu32 ",\"startup_quiet_s\":%" PRIu32 ",\"dealer_quiet_s\":%" PRIu32 ","
                 "\"dealer_context_s\":%" PRIu32 ",\"complete_on_user_events\":%" PRIu32 ",\"min_free_mb\":%" PRIu32 "},"
                 "\"tx_allow\":[",
                 p->id, p->name, p->bitrate, p->listen_only ? "true" : "false",
                 profile_name(p->logging.profile),
                 p->logging.startup_max_us / 1000000u, p->logging.startup_quiet_us / 1000000u,
                 p->logging.dealer_quiet_us / 1000000u, p->logging.dealer_context_us / 1000000u,
                 p->logging.complete_on_user_events, p->min_free_mb);
    if (n < 0 || n >= cap) {
        return -1;
    }
    for (i = 0; i < p->tx_allow_n && i < CANGO_TX_ALLOW_MAX; i++) {
        int w = snprintf(buf + n, (size_t)(cap - n), "%s%" PRIu32, i ? "," : "", p->tx_allow[i]);
        if (w < 0 || n + w >= cap) {
            return -1;
        }
        n += w;
    }
    if (n + 32 >= cap) {
        return -1;
    }
    memcpy(buf + n, "],\"modules\":[],\"actions\":[]}\n", 29);
    n += 29;
    buf[n] = 0;
    return n;
}

bool cango_tx_allowed(const cango_project_t *p, uint32_t id)
{
    uint8_t i;
    if (p == NULL) {
        return false;
    }
    for (i = 0; i < p->tx_allow_n && i < CANGO_TX_ALLOW_MAX; i++) {
        if (p->tx_allow[i] == id) {
            return true;
        }
    }
    return false;
}
