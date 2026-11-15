#include "cango_batch.h"
#include "cango_capture.h"
#include "cango_filter.h"
#include "cango_gvret.h"
#include "cango_log.h"
#include "cango_mode.h"
#include "cango_path.h"
#include "cango_project.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static cango_frame_t fr(uint32_t t, uint32_t id, uint8_t b0)
{
    cango_frame_t f;
    memset(&f, 0, sizeof(f));
    f.t_us = t;
    f.id = id;
    f.len = 8;
    f.data[0] = b0;
    return f;
}

static cango_frame_t isotp(uint32_t t, uint32_t id, uint8_t pci, uint8_t a, uint8_t b)
{
    cango_frame_t f = fr(t, id, pci);
    f.data[1] = a;
    f.data[2] = b;
    f.len = 3;
    return f;
}

/* ---- log ---- */

static int mem_write(void *ctx, const void *data, size_t len)
{
    struct {
        uint8_t buf[1024];
        size_t n;
        int flushes;
    } *m = ctx;
    if (data == NULL) {
        m->flushes++;
        return 0;
    }
    if (m->n + len > sizeof(m->buf)) {
        return -1;
    }
    memcpy(m->buf + m->n, data, len);
    m->n += len;
    return 0;
}

static void test_log(void)
{
    struct {
        uint8_t buf[1024];
        size_t n;
        int flushes;
    } mem;
    cango_log_info_t info;
    cango_log_t *log;
    cango_log_header_t hdr;
    cango_frame_t f = fr(100, 0x123, 0x11);
    memset(&mem, 0, sizeof(mem));
    memset(&info, 0, sizeof(info));
    info.bitrate = 500000;
    info.listen_only = 1;
    info.profile = CANGO_PROFILE_DEALER;
    memcpy(info.project_id, "generic", 7);
    info.start_us = 5;
    log = cango_log_open(mem_write, mem_write, &mem, &info);
    expect(log != NULL, "log open");
    expect(cango_log_frame(log, &f) == 0, "log frame");
    expect(cango_log_user_event(log, 42) == 0, "log user");
    expect(cango_log_flush(log) == 0, "log flush");
    expect(mem.flushes == 1, "flush called");
    cango_log_close(log);
    expect(mem.flushes == 2, "flush on close");
    expect(cango_log_parse_header(mem.buf, mem.n, &hdr) == 0, "header");
    expect(hdr.bitrate == 500000, "bitrate");
    expect(hdr.profile == CANGO_PROFILE_DEALER, "profile");
    expect(strcmp(hdr.project_id, "generic") == 0, "project id");
    expect(mem.buf[CANGO_LOG_HEADER_BYTES] == CANGO_REC_FRAME, "frame rec");
}

/* ---- mode ---- */

static void test_mode(void)
{
    cango_button_t b;
    cango_debounce_t d;
    expect(cango_mode_next(CANGO_MODE_CAPTURE) == CANGO_MODE_BLE, "next ble");
    expect(cango_mode_next(CANGO_MODE_BLE) == CANGO_MODE_WIFI, "next wifi");
    expect(cango_mode_next(CANGO_MODE_WIFI) == CANGO_MODE_CAPTURE, "next cap");

    cango_button_init(&b);
    expect(cango_button_feed(&b, true, 0) == CANGO_BTN_NONE, "boot ignores down");
    expect(cango_button_feed(&b, true, 1000) == CANGO_BTN_NONE, "boot ignores hold");
    cango_button_end_boot(&b, true);
    expect(cango_button_feed(&b, true, 2000) == CANGO_BTN_NONE, "held across boot swallowed");
    expect(cango_button_feed(&b, false, 2100) == CANGO_BTN_NONE, "release after boot not short");
    expect(cango_button_feed(&b, true, 3000) == CANGO_BTN_NONE, "press");
    expect(cango_button_feed(&b, false, 3400) == CANGO_BTN_SHORT, "short");
    expect(cango_button_feed(&b, true, 4000) == CANGO_BTN_NONE, "press2");
    expect(cango_button_feed(&b, false, 4700) == CANGO_BTN_NONE, "mid hold ignored");
    expect(cango_button_feed(&b, true, 5000) == CANGO_BTN_NONE, "press3");
    expect(cango_button_feed(&b, true, 5800) == CANGO_BTN_LONG, "long");
    expect(cango_button_feed(&b, true, 7000) == CANGO_BTN_NONE, "long once");
    expect(cango_button_feed(&b, false, 7100) == CANGO_BTN_NONE, "release after long");

    cango_debounce_init(&d);
    cango_debounce_end_boot(&d);
    expect(cango_debounce_feed(&d, true, 0) == CANGO_BTN_NONE, "debounce start");
    expect(cango_debounce_feed(&d, false, 10) == CANGO_BTN_NONE, "bounce");
    expect(cango_debounce_feed(&d, true, 20) == CANGO_BTN_NONE, "bounce2");
    expect(cango_debounce_feed(&d, true, 70) == CANGO_BTN_NONE, "stable down");
    expect(cango_debounce_feed(&d, false, 80) == CANGO_BTN_NONE, "raw up");
    expect(cango_debounce_feed(&d, false, 130) == CANGO_BTN_SHORT, "debounced short");
}

/* ---- filter ---- */

static void test_filter(void)
{
    cango_filter_t f;
    cango_filter_state_t st;
    cango_frame_t a, b;
    memset(&st, 0, sizeof(st));
    cango_filter_clear(&f);
    a = fr(1000, 0x100, 1);
    expect(cango_filter_match(&f, &st, &a), "empty passes");
    f.active = true;
    f.id = 0x100;
    f.mask = 0x7ff;
    f.extended = CANGO_EXT_STD;
    expect(cango_filter_match(&f, &st, &a), "id match");
    b = fr(2000, 0x101, 1);
    expect(!cango_filter_match(&f, &st, &b), "id miss");
    a.flags = CANGO_F_EXT;
    expect(!cango_filter_match(&f, &st, &a), "std rejects ext");
    a.flags = 0;
    f.change_only = true;
    memset(&st, 0, sizeof(st));
    a = fr(1000, 0x100, 1);
    expect(cango_filter_match(&f, &st, &a), "change first");
    a.t_us = 2000;
    expect(!cango_filter_match(&f, &st, &a), "change same");
    a.t_us = 3000;
    a.data[0] = 2;
    expect(cango_filter_match(&f, &st, &a), "change diff");
    f.change_only = false;
    f.min_interval_ms = 10;
    memset(&st, 0, sizeof(st));
    a = fr(0, 0x100, 1);
    expect(cango_filter_match(&f, &st, &a), "interval first");
    a.t_us = 5000;
    expect(!cango_filter_match(&f, &st, &a), "interval block");
    a.t_us = 10000;
    expect(cango_filter_match(&f, &st, &a), "interval pass");
    f.data_n = 1;
    f.data[0].index = 0;
    f.data[0].value = 0x22;
    f.data[0].mask = 0xff;
    f.min_interval_ms = 0;
    a = fr(20000, 0x100, 0x22);
    expect(cango_filter_match(&f, &st, &a), "data match");
    a.data[0] = 0x23;
    expect(!cango_filter_match(&f, &st, &a), "data miss");

    cango_filter_clear(&f);
    f.active = true;
    f.mode = CANGO_FILTER_MODE_ALL;
    f.id_n = 1;
    f.ids[0] = 0x100;
    a = fr(1, 0x100, 1);
    b = fr(2, 0x200, 1);
    expect(!cango_filter_match(&f, &st, &a), "hide listed");
    expect(cango_filter_match(&f, &st, &b), "all passes rest");
    f.mode = CANGO_FILTER_MODE_ONLY;
    expect(cango_filter_match(&f, &st, &a), "only listed");
    expect(!cango_filter_match(&f, &st, &b), "only misses");
    f.has_range = true;
    f.id_from = 0x300;
    f.id_to = 0x310;
    b = fr(3, 0x305, 1);
    expect(cango_filter_match(&f, &st, &b), "range inclusive");
    b = fr(4, 0x311, 1);
    expect(!cango_filter_match(&f, &st, &b), "range end");
    f.active = false;
    expect(cango_filter_match(&f, &st, &b), "inactive passes");
}

static int g_tx_n;
static cango_frame_t g_tx_frame;

static bool allow_all(uint32_t id, void *ctx)
{
    (void)id;
    (void)ctx;
    return true;
}

static bool allow_none(uint32_t id, void *ctx)
{
    (void)id;
    (void)ctx;
    return false;
}

static bool tx_capture(const cango_frame_t *frame, void *ctx)
{
    (void)ctx;
    g_tx_n++;
    g_tx_frame = *frame;
    return true;
}

static void test_gvret(void)
{
    cango_gvret_link_t link;
    uint8_t out[128];
    uint8_t hello[] = {0xE7, 0xE7, 0xF1, 0x06, 0xF1, 0x0C, 0xF1, 0x01};
    uint8_t info[] = {0xF1, 0x07};
    uint8_t alive[] = {0xF1, 0x09};
    uint8_t ext[] = {0xF1, 0x0D};
    uint8_t setup[] = {0xF1, 0x05, 0x20, 0xA1, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t send[] = {0xF1, 0x00, 0x23, 0x01, 0x00, 0x00, 0x00, 0x01, 0x11, 0x00};
    uint8_t send_bus[] = {0xF1, 0x00, 0x23, 0x01, 0x00, 0x00, 0x01, 0x01, 0x11, 0x00};
    uint8_t send_ext[] = {0xF1, 0x00, 0x23, 0x01, 0x00, 0x80, 0x00, 0x01, 0x22, 0x00};
    cango_frame_t known;
    uint8_t enc[32];
    int n;
    int el;

    cango_gvret_init(&link);
    n = cango_gvret_feed(&link, hello, (int)sizeof(hello), 1000, 500000, true, false, allow_all, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(n == 12 + 3 + 6, "handshake len");
    expect(out[0] == 0xF1 && out[1] == 0x06 && out[2] == 0x11, "listen flags");
    expect(out[3] == 0x20 && out[4] == 0xA1 && out[5] == 0x07 && out[6] == 0x00, "500k");
    expect(out[7] == 0 && out[11] == 0, "second bus off");
    expect(out[12] == 0xF1 && out[13] == 0x0C && out[14] == 0x01, "one bus");
    expect(out[15] == 0xF1 && out[16] == 0x01 && out[17] == 0xE8 && out[18] == 0x03, "time 1000");

    n = cango_gvret_feed(&link, info, (int)sizeof(info), 1000, 500000, true, false, allow_all, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(n == 8 && out[0] == 0xF1 && out[1] == 0x07 && out[2] == 0x01 && out[3] == 0x00, "device info");
    n = cango_gvret_feed(&link, alive, 2, 1000, 500000, true, false, allow_all, NULL, tx_capture, NULL, out, (int)sizeof(out));
    expect(n == 2 && out[0] == 0xF1 && out[1] == 0x09, "keepalive");
    n = cango_gvret_feed(&link, ext, 2, 1000, 500000, true, false, allow_all, NULL, tx_capture, NULL, out, (int)sizeof(out));
    expect(n == 17 && out[0] == 0xF1 && out[1] == 0x0D && out[16] == 0, "ext buses");

    n = cango_gvret_feed(&link, setup, (int)sizeof(setup), 1000, 250000, false, true, allow_all, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(n == 0, "set bitrate no reply");
    n = cango_gvret_feed(&link, alive, 2, 5, 250000, false, true, allow_all, NULL, tx_capture, NULL, out, (int)sizeof(out));
    expect(n == 2 && out[1] == 0x09, "parser stayed in sync");

    known = fr(1000, 0x123, 0xAB);
    known.len = 2;
    known.data[1] = 0xCD;
    el = cango_gvret_encode_frame(enc, (int)sizeof(enc), &known);
    expect(el == 14, "frame len");
    expect(enc[0] == 0xF1 && enc[1] == 0x00, "frame cmd");
    expect(enc[2] == 0xE8 && enc[3] == 0x03 && enc[4] == 0 && enc[5] == 0, "frame time");
    expect(enc[6] == 0x23 && enc[7] == 0x01 && enc[8] == 0 && enc[9] == 0, "frame id");
    expect(enc[10] == 0x02 && enc[11] == 0xAB && enc[12] == 0xCD && enc[13] == 0x00, "frame data");

    g_tx_n = 0;
    link.tx_calls = 0;
    n = cango_gvret_feed(&link, send, (int)sizeof(send), 5, 500000, true, false, allow_all, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(n == 0 && g_tx_n == 0 && link.tx_calls == 0 && link.tx_blocked == 1, "disarmed no tx");

    g_tx_n = 0;
    link.tx_calls = 0;
    link.tx_blocked = 0;
    n = cango_gvret_feed(&link, send, (int)sizeof(send), 5, 500000, false, true, allow_none, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(g_tx_n == 0 && link.tx_calls == 0 && link.tx_blocked == 1, "allow rejects");

    g_tx_n = 0;
    memset(&g_tx_frame, 0, sizeof(g_tx_frame));
    n = cango_gvret_feed(&link, send, (int)sizeof(send), 5, 500000, false, true, allow_all, NULL, tx_capture, NULL, out,
                         (int)sizeof(out));
    expect(g_tx_n == 1 && g_tx_frame.id == 0x123 && g_tx_frame.len == 1 && g_tx_frame.data[0] == 0x11, "armed tx");
    expect((g_tx_frame.flags & CANGO_F_TX) != 0 && (g_tx_frame.flags & CANGO_F_EXT) == 0, "tx flag");

    g_tx_n = 0;
    n = cango_gvret_feed(&link, send_bus, (int)sizeof(send_bus), 5, 500000, false, true, allow_all, NULL, tx_capture, NULL,
                         out, (int)sizeof(out));
    expect(g_tx_n == 0, "wrong bus");

    g_tx_n = 0;
    memset(&g_tx_frame, 0, sizeof(g_tx_frame));
    n = cango_gvret_feed(&link, send_ext, (int)sizeof(send_ext), 5, 500000, false, true, allow_all, NULL, tx_capture, NULL,
                         out, (int)sizeof(out));
    expect(g_tx_n == 1 && g_tx_frame.id == 0x123 && (g_tx_frame.flags & CANGO_F_EXT) != 0, "ext id");
    (void)n;
}

/* ---- batch ---- */

static void test_batch(void)
{
    cango_frame_t in[2];
    cango_frame_t out[2];
    uint8_t buf[64];
    uint16_t seq = 0;
    uint8_t n = 0;
    int len;
    in[0] = fr(1, 0x42, 9);
    in[1] = fr(2, 0x43, 8);
    in[1].flags = CANGO_F_TX;
    len = cango_batch_encode(buf, sizeof(buf), 7, in, 2);
    expect(len == (int)cango_batch_size(2), "batch len");
    expect(cango_batch_decode(buf, (size_t)len, &seq, out, 2, &n) == 0, "batch decode");
    expect(seq == 7 && n == 2, "batch seq");
    expect(out[1].id == 0x43 && (out[1].flags & CANGO_F_TX), "batch tx flag");
    expect(cango_batch_decode(buf, 3, &seq, out, 2, &n) != 0, "batch short");
}

/* ---- detect + capture ---- */

typedef struct {
    int open_dealer;
    int close_dealer;
    int open_startup;
    int close_startup;
    int frames;
    int events;
    int complete;
    int manual;
} acts_t;

static void on_act(void *ctx, int action, const cango_frame_t *frame, uint32_t aux)
{
    acts_t *a = ctx;
    (void)frame;
    (void)aux;
    if (action == CANGO_CAP_OPEN_DEALER) {
        a->open_dealer++;
    } else if (action == CANGO_CAP_CLOSE_DEALER) {
        a->close_dealer++;
    } else if (action == CANGO_CAP_OPEN_STARTUP) {
        a->open_startup++;
    } else if (action == CANGO_CAP_CLOSE_STARTUP) {
        a->close_startup++;
    } else if (action == CANGO_CAP_WRITE_FRAME) {
        a->frames++;
    } else if (action == CANGO_CAP_WRITE_EVENT) {
        a->events++;
    } else if (action == CANGO_CAP_COMPLETE) {
        a->complete++;
    } else if (action == CANGO_CAP_OPEN_MANUAL) {
        a->manual++;
    }
}

static cango_capture_cfg_t cfg_profile(uint8_t profile)
{
    cango_capture_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.profile = profile;
    c.startup_max_us = 1000000;
    c.startup_quiet_us = 3000;
    c.dealer_quiet_us = 5000;
    c.dealer_context_us = 1000000;
    return c;
}

static void test_dealer(void)
{
    cango_capture_t *c = cango_capture_new(&(cango_capture_cfg_t){0});
    cango_capture_cfg_t cfg = cfg_profile(CANGO_PROFILE_DEALER);
    acts_t a;
    int i;
    cango_frame_t f;
    cango_capture_free(c);
    c = cango_capture_new(&cfg);
    memset(&a, 0, sizeof(a));
    cango_capture_begin(c, 0, on_act, &a);
    for (i = 0; i < 20; i++) {
        f = fr((uint32_t)i * 10000u, 0x123, 0x10);
        cango_capture_on_frame(c, &f, on_act, &a);
    }
    cango_capture_poll(c, 200000, on_act, &a);
    expect(a.open_dealer == 0, "periodic does not open dealer");
    expect(!cango_capture_complete(c), "dealer stays purple");
    f = isotp(300000, 0x7E0, 0x03, 0x22, 0xF1);
    /* pci single with n=3 needs len >= 4. isotp() sets len 3 and pci byte as given.
     * 0x03 means single frame length 3, so len must be >= 4. Fix by using 0x02. */
    f = isotp(300000, 0x7E0, 0x02, 0x10, 0x01);
    cango_capture_on_frame(c, &f, on_act, &a);
    expect(a.open_dealer == 1, "request opens dealer");
    expect(a.frames >= 1, "dealer writes");
    f = isotp(300200, 0x7E8, 0x02, 0x50, 0x01);
    cango_capture_on_frame(c, &f, on_act, &a);
    cango_capture_poll(c, 300200 + 4999, on_act, &a);
    expect(a.close_dealer == 0, "quiet not yet");
    cango_capture_poll(c, 300200 + 5000, on_act, &a);
    expect(a.close_dealer == 1, "dealer closes");
    expect(a.complete == 1, "dealer complete");
    expect(cango_capture_complete(c), "white");
    cango_capture_on_user(c, 400000, on_act, &a);
    expect(a.manual == 1, "white short press opens manual");
    cango_capture_free(c);
}

static void test_startup(void)
{
    cango_capture_cfg_t cfg = cfg_profile(CANGO_PROFILE_STARTUP);
    cango_capture_t *c = cango_capture_new(&cfg);
    acts_t a;
    cango_frame_t f;
    const cango_census_t *census;
    memset(&a, 0, sizeof(a));
    cango_capture_begin(c, 0, on_act, &a);
    expect(a.open_startup == 1, "startup opens");
    f = fr(1000, 0x200, 1);
    cango_capture_on_frame(c, &f, on_act, &a);
    f = fr(2000, 0x200, 2);
    cango_capture_on_frame(c, &f, on_act, &a);
    cango_capture_on_user(c, 2500, on_act, &a);
    expect(a.events == 1, "user event in startup");
    cango_capture_poll(c, 2000 + 3000, on_act, &a);
    expect(a.close_startup == 1, "startup quiet close");
    expect(a.complete == 1, "startup complete without tester");
    census = cango_capture_census(c);
    expect(census != NULL && census->rows[0].used && census->rows[0].count == 2, "census count");
    expect(census->rows[0].changed_mask & 0x01, "census changed byte0");
    cango_capture_free(c);
}

static void test_both(void)
{
    cango_capture_cfg_t cfg = cfg_profile(CANGO_PROFILE_BOTH);
    cango_capture_t *c = cango_capture_new(&cfg);
    acts_t a;
    cango_frame_t f;
    memset(&a, 0, sizeof(a));
    cango_capture_begin(c, 0, on_act, &a);
    f = fr(1000, 0x10, 1);
    cango_capture_on_frame(c, &f, on_act, &a);
    cango_capture_poll(c, 1000 + 3000, on_act, &a);
    expect(a.close_startup == 1, "both startup closed");
    expect(a.complete == 0, "both waits for dealer");
    f = isotp(10000, 0x7DF, 0x02, 0x3E, 0x00);
    cango_capture_on_frame(c, &f, on_act, &a);
    cango_capture_poll(c, 10000 + 5000, on_act, &a);
    expect(a.complete == 1, "both completes");
    cango_capture_free(c);
}

static void test_project(void)
{
    cango_project_t p;
    cango_project_t q;
    char buf[512];
    cango_project_generic(&p);
    expect(p.logging.profile == CANGO_PROFILE_DEALER, "generic dealer");
    expect(p.bitrate == 500000, "generic bitrate");
    expect(cango_project_write(&p, buf, (int)sizeof(buf)) > 0, "write project");
    expect(cango_project_parse(buf, &q), "parse project");
    expect(strcmp(q.id, "generic") == 0, "id");
    expect(q.logging.profile == CANGO_PROFILE_DEALER, "parsed dealer");
    expect(cango_project_parse("{\"capture_profile\":\"startup\"}", &q), "startup profile");
    expect(q.logging.profile == CANGO_PROFILE_STARTUP, "startup set");
    expect(cango_project_parse("{\"capture_profile\":\"nope\"}", &q) == false, "bad profile");
    expect(cango_project_parse("{\"tx_allow\":[291, 123]}", &q), "tx allow");
    expect(q.tx_allow_n == 2 && cango_tx_allowed(&q, 291) && !cango_tx_allowed(&q, 1), "allow list");
    p.tx_allow_n = 1;
    p.tx_allow[0] = 0x100;
    expect(cango_project_write(&p, buf, (int)sizeof(buf)) > 0, "write allow");
    expect(cango_project_parse(buf, &q) && cango_tx_allowed(&q, 0x100), "roundtrip allow");
    expect(cango_project_builtin("zero-xb", &p), "builtin xb");
    expect(strcmp(p.id, "zero-xb") == 0 && strcmp(p.name, "Zero XB") == 0, "xb name");
    expect(p.listen_only && p.tx_allow_n == 0, "xb listen empty allow");
    expect(p.logging.profile == CANGO_PROFILE_DEALER && p.bitrate == 500000, "xb dealer listen");
    expect(!cango_project_is_generic(p.id), "xb not generic");
    expect(cango_project_builtin("missing-bike", &q) == false, "unknown builtin");
    expect(cango_project_write(&p, buf, (int)sizeof(buf)) > 0, "write xb");
    expect(cango_project_parse(buf, &q), "parse xb");
    expect(strcmp(q.id, "zero-xb") == 0 && q.tx_allow_n == 0 && q.listen_only, "xb roundtrip");
    expect(q.logging.profile == CANGO_PROFILE_DEALER, "xb profile kept");
    expect(cango_project_builtin("generic", &q) && cango_project_is_generic(q.id), "builtin generic");
}

static void test_path(void)
{
    char buf[64];
    expect(cango_path_under(buf, sizeof(buf), "/sdcard", ""), "root");
    expect(strcmp(buf, "/sdcard") == 0, "root text");
    expect(cango_path_under(buf, sizeof(buf), "/sdcard", "logs/12"), "nested");
    expect(strcmp(buf, "/sdcard/logs/12") == 0, "nested text");
    expect(cango_path_under(buf, sizeof(buf), "/sdcard", "logs//12/"), "slashes");
    expect(strcmp(buf, "/sdcard/logs/12") == 0, "collapsed");
    expect(!cango_path_under(buf, sizeof(buf), "/sdcard", ".."), "dotdot");
    expect(!cango_path_under(buf, sizeof(buf), "/sdcard", "logs/../../etc"), "escape");
    expect(!cango_path_under(buf, sizeof(buf), "/sdcard", "/etc"), "absolute");
    expect(!cango_path_under(buf, sizeof(buf), "/sdcard", "a\\b"), "backslash");
    expect(!cango_path_name_ok("..") && cango_path_name_ok("log.cango"), "names");
    expect(cango_path_hits("/sdcard/logs/1", "/sdcard/logs/1"), "same");
    expect(cango_path_hits("/sdcard/logs/1", "/sdcard/logs/1/log.cango"), "child");
    expect(cango_path_hits("/sdcard/logs/1", "/sdcard/logs"), "parent");
    expect(!cango_path_hits("/sdcard/logs/1", "/sdcard/logs/10"), "prefix");
    expect(!cango_path_hits("", "/sdcard/logs"), "idle");
}

int main(void)
{
    test_log();
    test_mode();
    test_filter();
    test_gvret();
    test_batch();
    test_dealer();
    test_startup();
    test_both();
    test_project();
    test_path();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("cango core tests passed\n");
    return 0;
}
