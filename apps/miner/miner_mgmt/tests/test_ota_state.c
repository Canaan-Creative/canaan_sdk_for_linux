// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#include "ota_state.h"

#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) \
    do { \
        if (c) { \
            printf("  ok: "); \
        } else { \
            fails++; \
            printf("  FAIL: "); \
        } \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } while (0)

#define EV_CAP   512  /* Size of rec.ev, bytes */
#define DATA_CAP 8192 /* Size of rec.data, bytes */
#define IMG_CAP  8192 /* Size of the package image buffer g_img, bytes */

/* Sink call recorder, also carrying the fault and return-code knobs */
typedef struct {
    /* Event trace: "b<i>", "w<i>", "e<i>" per begin/write/end of bin i, space separated */
    char          ev[EV_CAP];
    size_t        evlen;
    char          last_kind;      /* Kind of the last event, 'b', 'w' or 'e' */
    int           last_idx;       /* Bin index of the last event */
    unsigned char data[DATA_CAP]; /* Every written payload byte, concatenated */
    size_t        datalen;
    int           begins;
    int           writes;
    int           ends;
    int           hdrs;      /* check_hdr calls */
    int           parts;     /* check_part calls */
    int           hdr_bins;  /* totalbins of the header passed to check_hdr */
    int           overflow;  /* ev or data ran out of room */
    int           discontig; /* A write skipped or rewound within its bin */
    int           zerolen;   /* A zero-length write was seen */
    int           badbin;    /* A bin name was not "bin<i>" with i inside AUP_MAX_BINS */
    int           badsize;   /* A bin's header size differed from want_size */
    long long     next_off[AUP_MAX_BINS];  /* Offset the next write of each bin is expected at */
    int32_t       want_size[AUP_MAX_BINS]; /* Expected header size of each bin, bytes */
    int           fail_begin;              /* 1-based begin call that fails, 0 none */
    int           fail_write;              /* 1-based write call that fails, 0 none */
    int           fail_end;                /* 1-based end call that fails, 0 none */
    int           hdr_rc;                  /* Value check_hdr returns */
    int           part_rc;                 /* Value check_part returns */
} rec;

static int fails;
static int g_sig_rc; /* Value aup_verify_signature() returns */

/* Package image fed to ota_feed(): header, then each bin's payload */
static unsigned char g_img[IMG_CAP];

int aup_verify_signature(const struct aup_hdr *h)
{
    (void)h;
    return g_sig_rc;
}

static void ev_add(rec *r, char kind, int idx)
{
    if (kind == 'w' && r->last_kind == 'w' && r->last_idx == idx)
        return;
    if (r->evlen + 16 >= sizeof r->ev) {
        r->overflow = 1;
        return;
    }
    r->evlen += (size_t)snprintf(r->ev + r->evlen, sizeof r->ev - r->evlen, "%s%c%d",
            r->evlen ? " " : "", kind, idx);
    r->last_kind = kind;
    r->last_idx  = idx;
}

static int bin_index(const struct aup_bin *b)
{
    if (b == NULL || strncmp(b->name, "bin", 3) != 0)
        return -1;
    return atoi(b->name + 3);
}

static int slot_of(rec *r, const struct aup_bin *b)
{
    int i = bin_index(b);

    if (i < 0 || i >= AUP_MAX_BINS) {
        r->badbin = 1;
        return 0;
    }
    if (aup_be32(b->size) != r->want_size[i])
        r->badsize = 1;
    return i;
}

static int s_check_hdr(void *ctx, const struct aup_hdr *h)
{
    rec *r = (rec *)ctx;

    r->hdrs++;
    r->hdr_bins = (int)aup_be32(h->totalbins);
    return r->hdr_rc;
}

static int s_check_part(void *ctx, const struct aup_hdr *h)
{
    rec *r = (rec *)ctx;

    (void)h;
    r->parts++;
    return r->part_rc;
}

static int s_begin(void *ctx, const struct aup_bin *b)
{
    rec *r = (rec *)ctx;
    int  i = slot_of(r, b);

    r->begins++;
    ev_add(r, 'b', i);
    return (r->fail_begin == r->begins) ? -1 : 0;
}

static int s_write(void *ctx, const struct aup_bin *b, long long off, const void *data, size_t len)
{
    rec *r = (rec *)ctx;
    int  i = slot_of(r, b);

    r->writes++;
    if (off != r->next_off[i])
        r->discontig = 1;
    r->next_off[i] = off + (long long)len;
    if (len == 0)
        r->zerolen = 1;
    if (r->datalen + len <= sizeof r->data) {
        memcpy(r->data + r->datalen, data, len);
        r->datalen += len;
    } else {
        r->overflow = 1;
    }
    ev_add(r, 'w', i);
    return (r->fail_write == r->writes) ? -1 : 0;
}

static int s_end(void *ctx, const struct aup_bin *b)
{
    rec *r = (rec *)ctx;
    int  i = slot_of(r, b);

    r->ends++;
    ev_add(r, 'e', i);
    return (r->fail_end == r->ends) ? -1 : 0;
}

static void rec_init(rec *r, const int32_t *sizes, int n)
{
    int i;

    memset(r, 0, sizeof *r);
    for (i = 0; i < n && i < AUP_MAX_BINS; i++)
        r->want_size[i] = sizes[i];
}

static ota_sink mk_sink(rec *r)
{
    ota_sink s;

    s.begin      = s_begin;
    s.write      = s_write;
    s.end        = s_end;
    s.ctx        = r;
    s.check_hdr  = s_check_hdr;
    s.check_part = s_check_part;
    return s;
}

static int rec_same(const rec *a, const rec *b)
{
    return strcmp(a->ev, b->ev) == 0 && a->datalen == b->datalen &&
            memcmp(a->data, b->data, a->datalen) == 0 && a->begins == b->begins &&
            a->ends == b->ends && a->hdrs == b->hdrs && a->parts == b->parts;
}

static void hdr_init(struct aup_hdr *h, const int32_t *sizes, int n)
{
    int32_t off   = AUP_HDR_SIZE;
    int32_t total = 0;
    int     i;

    memset(h, 0, sizeof *h);
    h->format = aup_be32(AUP_FORMAT);
    memcpy(h->manufacturer, "canaan", 7);
    memcpy(h->product, "nano3s", 7);
    memcpy(h->mainctroler, AUP_MAINCTROLER, strlen(AUP_MAINCTROLER) + 1);
    memcpy(h->time, "2026-09-14 00:00:00", 20);
    memcpy(h->version, "0.4.0", 6);
    memcpy(h->hw, "hw1", 4);
    memcpy(h->sw, "sw1", 4);
    memcpy(h->upgrade_type, AUP_UPGRADE_TYPE, strlen(AUP_UPGRADE_TYPE) + 1);
    h->totalbins = aup_be32(n);
    for (i = 0; i < n && i < AUP_MAX_BINS; i++) {
        snprintf(h->f[i].name, sizeof h->f[i].name, "bin%d", i);
        h->f[i].ofst     = aup_be32(off);
        h->f[i].size     = aup_be32(sizes[i]);
        h->f[i].partofst = aup_be32(0);
        h->f[i].partsize = aup_be32(sizes[i] > 0 ? sizes[i] : 1);
        if (sizes[i] > 0) {
            off += sizes[i];
            total += sizes[i];
        }
    }
    h->totalsize = aup_be32(total);
}

static size_t build(struct aup_hdr *h, const int32_t *sizes, int n)
{
    size_t  pos = AUP_HDR_SIZE;
    int     i;
    int32_t k;

    memset(g_img, 0, sizeof g_img);
    for (i = 0; i < n; i++) {
        size_t start = pos;

        for (k = 0; k < sizes[i] && pos < sizeof g_img; k++)
            g_img[pos++] = (unsigned char)(0x30 + i * 7 + (k % 251));
        if (i < AUP_MAX_BINS)
            sha256(g_img + start, pos - start, h->f[i].sha256);
    }
    memcpy(g_img, h, sizeof *h);
    return pos;
}

static int feed_fixed(ota_state_t *st, const unsigned char *img, size_t len, size_t chunk,
        int8_t *pct, int *mono)
{
    size_t pos  = 0;
    int    rc   = UPGRADE_PROCESS_OK;
    int8_t p    = 0;
    int8_t prev = 0;

    *mono = 1;
    while (pos < len) {
        size_t n = len - pos;

        if (n > chunk)
            n = chunk;
        rc = ota_feed(st, img + pos, (int)n, &p);
        if (p < prev)
            *mono = 0;
        prev = p;
        pos += n;
        if (rc != UPGRADE_PROCESS_OK)
            break;
    }
    *pct = p;
    return rc;
}

static int feed_list(ota_state_t *st, const unsigned char *img, size_t len, const size_t *chunks,
        int nch, int8_t *pct, int *mono)
{
    size_t pos  = 0;
    int    rc   = UPGRADE_PROCESS_OK;
    int8_t p    = 0;
    int8_t prev = 0;
    int    i;

    *mono = 1;
    for (i = 0; i <= nch && pos < len; i++) {
        size_t n = (i < nch) ? chunks[i] : len - pos;

        if (n > len - pos)
            n = len - pos;
        rc = ota_feed(st, img + pos, (int)n, &p);
        if (p < prev)
            *mono = 0;
        prev = p;
        pos += n;
        if (rc != UPGRADE_PROCESS_OK)
            break;
    }
    *pct = p;
    return rc;
}

static int feed_whole(ota_state_t *st, rec *r, size_t len, const int32_t *sizes, int n, int8_t *pct)
{
    ota_sink sk;

    rec_init(r, sizes, n);
    sk = mk_sink(r);
    ota_reset(st, &sk);
    *pct = 0;
    return ota_feed(st, g_img, (int)len, pct);
}

static void split_case(ota_state_t *st, size_t len, const int32_t *sizes, int n, const rec *base,
        size_t chunk, const size_t *chunks, int nch, const char *label)
{
    rec      r;
    ota_sink sk;
    int8_t   pct  = 0;
    int      mono = 0;
    int      rc;

    rec_init(&r, sizes, n);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = (chunks != NULL) ? feed_list(st, g_img, len, chunks, nch, &pct, &mono) :
                            feed_fixed(st, g_img, len, chunk, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED, "%s: the image is accepted (rc=%d)", label, rc);
    CHECK(rec_same(&r, base), "%s: identical sink call sequence [%s] against [%s]", label, r.ev,
            base->ev);
    CHECK(!r.discontig && !r.badbin && !r.badsize && !r.zerolen && !r.overflow,
            "%s: every write is contiguous from offset 0 and carries its own bin "
            "descriptor, with no zero-length write",
            label);
    CHECK(mono && pct == 100, "%s: percent is monotonic and ends at 100 (got %d)", label, (int)pct);
}

int main(void)
{
    static const int32_t sz3[3]             = { 5, 1000, 700 };
    static const int32_t sz1[1]             = { 64 };
    static const int32_t sz14[AUP_MAX_BINS] = { 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };
    static const int32_t szz[3]             = { 600, 0, 700 };
    static const int32_t szn[3]             = { 600, -4, 700 };
    static const size_t  cuts_a[5]          = { 1000, 1048, 2, 850, 160 };
    static const size_t  cuts_b[1]          = { 2048 };
    static const size_t  cuts_c[2]          = { 2047, 1 };
    struct aup_hdr       h;
    struct aup_hdr       out;
    ota_state_t         *st;
    ota_sink             sk;
    rec                  base;
    rec                  r;
    size_t               len;
    int8_t               pct  = 0;
    int                  mono = 0;
    int                  rc;
    int                  i;

    st = (ota_state_t *)malloc(ota_state_size());
    CHECK(ota_state_size() > 0 && st != NULL,
            "ota_state_size() reports %zu bytes and the caller owns the allocation",
            ota_state_size());
    if (st == NULL)
        return 1;

    memset(&out, 0, sizeof out);
    hdr_init(&h, sz3, 3);
    len = build(&h, sz3, 3);
    CHECK(len == (size_t)AUP_HDR_SIZE + 1705,
            "the fixture is a 2048-byte header with three bins back-to-back (%zu bytes)", len);

    rc = aup_hdr_parse(g_img, len, &out);
    CHECK(rc == 0, "aup_hdr_parse accepts the fixture (rc=%d)", rc);
    CHECK(aup_be32(out.totalbins) == 3 && aup_be32(out.f[1].size) == 1000,
            "...and the big-endian integer fields decode through aup_be32");
    CHECK(aup_hdr_parse(g_img, AUP_HDR_SIZE - 1, &out) == UPGRADE_HEAER_ERR,
            "a short buffer is UPGRADE_HEAER_ERR, never a partial parse");
    CHECK(aup_hdr_parse(NULL, len, &out) == UPGRADE_HEAER_ERR,
            "a NULL buffer is UPGRADE_HEAER_ERR");
    CHECK(aup_hdr_parse(g_img, len, NULL) == UPGRADE_HEAER_ERR,
            "a NULL output is UPGRADE_HEAER_ERR");

    g_sig_rc = UPGRADE_VERIFY_ERR;
    CHECK(aup_hdr_parse(g_img, len, &out) == 0,
            "aup_hdr_parse gates format, mainctroler and upgrade_type only: the "
            "signature is not its business");
    rc = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_VERIFY_ERR,
            "a refused signature is returned verbatim as UPGRADE_VERIFY_ERR, "
            "never folded into UPGRADE_HEAER_ERR (%d)",
            rc);
    CHECK(r.hdrs == 1 && r.parts == 0 && r.begins == 0,
            "...after the sink's SW/HW gates have run but before its partition "
            "gate, and still before any bin is opened: no erase, no slot switch");
    g_sig_rc = 0;

    rec_init(&base, sz3, 3);
    sk = mk_sink(&base);
    ota_reset(st, &sk);
    rc = feed_fixed(st, g_img, len, len, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED, "one whole-image chunk succeeds (rc=%d)", rc);
    CHECK(strcmp(base.ev, "b0 w0 e0 b1 w1 e1 b2 w2 e2") == 0,
            "...opening, writing and closing each bin in order, once each [%s]", base.ev);
    CHECK(base.hdrs == 1 && base.parts == 1 && base.hdr_bins == 3,
            "...with the header offered to each of the sink's two gates exactly once");
    CHECK(base.datalen == 1705 && memcmp(base.data, g_img + AUP_HDR_SIZE, 1705) == 0,
            "...and the bytes handed to the sink are the image bytes, in order");
    CHECK(pct == 100 && mono, "...ending at percent 100 (got %d)", (int)pct);

    split_case(st, len, sz3, 3, &base, 1, NULL, 0, "1-byte chunks");
    split_case(st, len, sz3, 3, &base, 7, NULL, 0, "7-byte chunks");
    split_case(st, len, sz3, 3, &base, 1000, NULL, 0, "1000-byte chunks (header split twice)");
    split_case(st, len, sz3, 3, &base, len + 4096, NULL, 0, "a chunk larger than the whole image");
    split_case(st, len, sz3, 3, &base, 0, cuts_b, 1,
            "a chunk ending exactly on the header boundary");
    split_case(st, len, sz3, 3, &base, 0, cuts_c, 2, "a 1-byte chunk completing the header");
    split_case(st, len, sz3, 3, &base, 0, cuts_a, 5,
            "chunks carrying a bin tail plus the next bin's head");

    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, AUP_HDR_SIZE - 1, &pct);
    CHECK(rc == UPGRADE_PROCESS_OK, "2047 header bytes are in progress (rc=%d)", rc);
    CHECK(r.hdrs == 0 && r.begins == 0,
            "...and an incomplete header is offered to nothing: no erase, no slot switch");
    rc = ota_feed(st, g_img + AUP_HDR_SIZE - 1, 1, &pct);
    CHECK(rc == UPGRADE_PROCESS_OK && r.hdrs == 1, "the 2048th byte parses the header (rc=%d)", rc);
    CHECK(r.begins == 0 && r.writes == 0 && pct == 0,
            "...and still opens nothing: a header alone carries no image bytes");

    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, AUP_HDR_SIZE, &pct);
    CHECK(rc == UPGRADE_PROCESS_OK && r.hdrs == 1 && r.begins == 0,
            "a chunk ending exactly on the header boundary opens no bin either (rc=%d)", rc);
    rc = ota_feed(st, g_img + AUP_HDR_SIZE, (int)(len - AUP_HDR_SIZE), &pct);
    CHECK(rc == UPGRADE_SUCCEED && strcmp(r.ev, base.ev) == 0,
            "...and the payload that follows it replays the same sequence [%s]", r.ev);

    memcpy(g_img + len, "trailing-bytes-past-the-last-bin", 32);
    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)(len + 32), &pct);
    CHECK(rc == UPGRADE_SUCCEED, "bytes past the last bin still succeed (rc=%d)", rc);
    CHECK(r.datalen == 1705 && strcmp(r.ev, base.ev) == 0,
            "...and the trailing bytes are dropped, not written into a partition");
    CHECK(ota_feed(st, g_img, 4, &pct) == UPGRADE_ALREADY_ERR && r.begins == 3,
            "...and a feed after UPGRADE_SUCCEED is UPGRADE_ALREADY_ERR, not a "
            "restart at bin 0");
    CHECK(pct == 100, "...leaving percent at 100 (got %d)", (int)pct);

    hdr_init(&h, sz1, 1);
    len = build(&h, sz1, 1);
    rec_init(&r, sz1, 1);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = feed_fixed(st, g_img, len, 3, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED && strcmp(r.ev, "b0 w0 e0") == 0,
            "a single-bin image is one open/write/close (rc=%d, [%s])", rc, r.ev);
    CHECK(mono && pct == 100, "...and percent runs 0 to 100 monotonically (%d)", (int)pct);

    hdr_init(&h, sz14, AUP_MAX_BINS);
    len = build(&h, sz14, AUP_MAX_BINS);
    rec_init(&r, sz14, AUP_MAX_BINS);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = feed_fixed(st, g_img, len, 5, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED, "AUP_MAX_BINS bins is accepted (rc=%d)", rc);
    CHECK(r.begins == AUP_MAX_BINS && r.ends == AUP_MAX_BINS, "...opening and closing all 14");
    CHECK(mono && pct == 100, "...and 100/14 per bin still lands on exactly 100 (%d)", (int)pct);

    hdr_init(&h, sz3, 3);
    len = build(&h, sz3, 3);
    h.f[1].sha256[0] ^= 0xff;
    memcpy(g_img, &h, sizeof h);
    rc = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_CHECK_SHA_ERR,
            "a bin whose payload does not hash to its sha256 is "
            "UPGRADE_CHECK_SHA_ERR (%d)",
            rc);
    CHECK(strcmp(r.ev, "b0 w0 e0 b1 w1") == 0 && r.begins == 2 && r.ends == 1,
            "...caught before that bin's end, so the sink never commits it and the "
            "next bin is never opened [%s]",
            r.ev);
    CHECK(pct == 0, "...and the percent the first bin earned is dropped (got %d)", (int)pct);
    CHECK(ota_feed(st, g_img, 4, &pct) == UPGRADE_ALREADY_ERR,
            "...and a digest failure is final, not retried");

    hdr_init(&h, sz3, 3);
    h.format = aup_be32(AUP_FORMAT + 1);
    len      = build(&h, sz3, 3);
    rc       = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "a wrong format word is UPGRADE_HEAER_ERR (%d)", rc);
    CHECK(r.hdrs == 0 && r.begins == 0, "...rejected before the sink is consulted at all");

    hdr_init(&h, sz3, 3);
    memcpy(h.mainctroler, "k510", 5);
    len = build(&h, sz3, 3);
    rc  = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "another chip's mainctroler is UPGRADE_HEAER_ERR (%d)", rc);

    hdr_init(&h, sz3, 3);
    h.mainctroler[4] = 'x';
    len              = build(&h, sz3, 3);
    rc               = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR,
            "a mainctroler with 'k230' as a PREFIX is UPGRADE_HEAER_ERR (%d): the "
            "match is exact, terminator included",
            rc);

    hdr_init(&h, sz3, 3);
    memcpy(h.upgrade_type, "FULL", 5);
    len = build(&h, sz3, 3);
    rc  = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "an upgrade_type that is not OTA is UPGRADE_HEAER_ERR (%d)", rc);

    hdr_init(&h, sz3, 3);
    h.totalbins = aup_be32(0);
    len         = build(&h, sz3, 3);
    rc          = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "totalbins 0 is UPGRADE_HEAER_ERR (%d)", rc);
    CHECK(r.begins == 0, "...and cannot reach UPGRADE_SUCCEED with no bins");

    hdr_init(&h, sz3, 3);
    h.totalbins = aup_be32(AUP_MAX_BINS + 1);
    len         = build(&h, sz3, 3);
    rc          = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR,
            "totalbins 15 is UPGRADE_HEAER_ERR (%d): the descriptor array holds 14", rc);

    hdr_init(&h, sz3, 3);
    h.totalbins = aup_be32(-1);
    len         = build(&h, sz3, 3);
    rc          = feed_whole(st, &r, len, sz3, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "a negative totalbins is UPGRADE_HEAER_ERR (%d)", rc);

    hdr_init(&h, szz, 3);
    len = build(&h, szz, 3);
    rc  = feed_whole(st, &r, len, szz, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "a bin with size 0 is UPGRADE_HEAER_ERR (%d), not a silent skip",
            rc);
    CHECK(r.begins == 0 && r.writes == 0 && r.ends == 0,
            "...and NOTHING is opened: a zero-size bin must not cost an erase or an "
            "A/B slot switch");
    CHECK(ota_feed(st, g_img, 8, &pct) == UPGRADE_ALREADY_ERR,
            "...and the rejected stream stays rejected");

    hdr_init(&h, szn, 3);
    len = build(&h, szn, 3);
    rc  = feed_whole(st, &r, len, szn, 3, &pct);
    CHECK(rc == UPGRADE_HEAER_ERR, "a bin with a negative size is UPGRADE_HEAER_ERR (%d)", rc);
    CHECK(r.begins == 0, "...and opens nothing either");

    hdr_init(&h, sz3, 3);
    len = build(&h, sz3, 3);
    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, AUP_HDR_SIZE - 8, &pct);
    CHECK(rc == UPGRADE_PROCESS_OK && r.hdrs == 0,
            "an upload that stops inside the header never parses one (rc=%d)", rc);

    rec_init(&r, sz3, 3);
    r.hdr_rc = UPGRADE_HW_ERR;
    sk       = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_HW_ERR,
            "the sink's own check_hdr refusal is returned verbatim (%d), not folded "
            "into UPGRADE_IMAGE_ERR",
            rc);
    CHECK(r.hdrs == 1 && r.parts == 0 && r.begins == 0,
            "...consulted once, and neither the partition gate nor any bin is "
            "reached after it refuses");
    CHECK(ota_feed(st, g_img, (int)len, &pct) == UPGRADE_ALREADY_ERR,
            "...and the refusal is final");

    g_sig_rc = UPGRADE_VERIFY_ERR;
    rec_init(&r, sz3, 3);
    r.hdr_rc = UPGRADE_SW_ERR;
    sk       = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_SW_ERR,
            "a package that is BOTH for the wrong SW and badly signed reports the "
            "SW error, not UPGRADE_VERIFY_ERR (%d)",
            rc);
    CHECK(r.hdrs == 1 && r.begins == 0,
            "...so SW and HW are gated before the signature, and an unsigned "
            "package still reaches them");

    rec_init(&r, sz3, 3);
    r.part_rc = UPGRADE_PARTITION_ERR;
    sk        = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_VERIFY_ERR,
            "a package that is BOTH badly signed and sized for another flash "
            "layout reports UPGRADE_VERIFY_ERR, not UPGRADE_PARTITION_ERR (%d)",
            rc);
    CHECK(r.hdrs == 1 && r.parts == 0 && r.begins == 0,
            "...so the partition gate runs AFTER the signature, and an unsigned "
            "package never reaches it");
    g_sig_rc = 0;

    rec_init(&r, sz3, 3);
    r.part_rc = UPGRADE_PARTITION_ERR;
    sk        = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_PARTITION_ERR,
            "with a good signature the sink's check_part refusal is returned "
            "verbatim (%d), not folded into UPGRADE_IMAGE_ERR",
            rc);
    CHECK(r.parts == 1 && r.begins == 0,
            "...consulted once, and no bin is opened after it refuses");
    CHECK(ota_feed(st, g_img, (int)len, &pct) == UPGRADE_ALREADY_ERR,
            "...and that refusal is final too");

    rec_init(&r, sz3, 3);
    sk            = mk_sink(&r);
    sk.check_part = NULL;
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_SUCCEED,
            "a sink that binds check_hdr but leaves check_part NULL still runs (%d)", rc);

    rec_init(&r, sz3, 3);
    r.fail_begin = 1;
    sk           = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_IMAGE_ERR, "a failing begin is UPGRADE_IMAGE_ERR (%d)", rc);
    CHECK(strcmp(r.ev, "b0") == 0 && r.writes == 0 && r.ends == 0,
            "...and nothing is written; end is NOT called for a bin that never "
            "opened [%s]",
            r.ev);
    CHECK(pct == 0, "...with percent reported as 0 (got %d)", (int)pct);
    CHECK(ota_feed(st, g_img + AUP_HDR_SIZE, 16, &pct) == UPGRADE_ALREADY_ERR && r.begins == 1,
            "...and a further feed is UPGRADE_ALREADY_ERR, not a restart at bin 0");

    rec_init(&r, sz3, 3);
    r.fail_write = 2;
    sk           = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_IMAGE_ERR, "a failing write is UPGRADE_IMAGE_ERR (%d)", rc);
    CHECK(strcmp(r.ev, "b0 w0 e0 b1 w1") == 0 && r.ends == 1,
            "...and end is NOT called on the failing bin: end commits, and a bin "
            "whose write failed is abandoned by the caller instead [%s]",
            r.ev);
    CHECK(r.begins == 2, "...and the next bin is never opened");
    CHECK(pct == 0, "...and percent is reset to 0 from the 33 the first bin had earned (got %d)",
            (int)pct);
    CHECK(ota_feed(st, g_img, 4, &pct) == UPGRADE_ALREADY_ERR,
            "...and the failed stream cannot be continued");

    rec_init(&r, sz3, 3);
    r.fail_end = 1;
    sk         = mk_sink(&r);
    ota_reset(st, &sk);
    rc = ota_feed(st, g_img, (int)len, &pct);
    CHECK(rc == UPGRADE_IMAGE_ERR, "a failing end is UPGRADE_IMAGE_ERR (%d)", rc);
    CHECK(strcmp(r.ev, "b0 w0 e0") == 0 && r.begins == 1,
            "...and the bin after it is never opened [%s]", r.ev);
    CHECK(pct == 0, "...with percent reported as 0 (got %d)", (int)pct);

    rec_init(&r, sz3, 3);
    r.fail_end = 3;
    sk         = mk_sink(&r);
    ota_reset(st, &sk);
    rc = feed_fixed(st, g_img, len, 64, &pct, &mono);
    CHECK(rc == UPGRADE_IMAGE_ERR,
            "an end failing on the LAST bin is UPGRADE_IMAGE_ERR, not UPGRADE_SUCCEED (%d)", rc);
    CHECK(pct == 0, "...and the percent the first two bins earned is dropped (got %d)", (int)pct);

    sk.begin      = NULL;
    sk.write      = NULL;
    sk.end        = NULL;
    sk.check_hdr  = NULL;
    sk.check_part = NULL;
    sk.ctx        = NULL;
    ota_reset(st, &sk);
    rc = feed_fixed(st, g_img, len, 128, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED,
            "a sink with every callback NULL still drives the stream to the end (%d)", rc);
    CHECK(mono && pct == 100, "...and still reports 100 (got %d)", (int)pct);

    ota_reset(st, NULL);
    rc = feed_fixed(st, g_img, len, 128, &pct, &mono);
    CHECK(rc == UPGRADE_SUCCEED, "ota_reset with a NULL sink is the same (%d)", rc);

    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    CHECK(ota_feed(st, NULL, 4, &pct) == UPGRADE_IMAGE_ERR,
            "a NULL buffer with a positive length is UPGRADE_IMAGE_ERR");
    CHECK(ota_feed(st, g_img, -1, &pct) == UPGRADE_IMAGE_ERR,
            "a negative length is UPGRADE_IMAGE_ERR");
    CHECK(ota_feed(st, g_img, 0, &pct) == UPGRADE_PROCESS_OK,
            "an empty chunk is in progress, not an error");
    CHECK(r.hdrs == 0 && r.begins == 0, "...and none of those three touched the sink");
    rc = ota_feed(st, g_img, (int)len, NULL);
    CHECK(rc == UPGRADE_SUCCEED, "a NULL percent pointer is accepted (%d)", rc);
    CHECK(ota_feed(NULL, g_img, 4, &pct) == UPGRADE_IMAGE_ERR, "a NULL state is UPGRADE_IMAGE_ERR");

    rec_init(&r, sz3, 3);
    sk = mk_sink(&r);
    ota_reset(st, &sk);
    for (i = 0, mono = 1; i < (int)len - 1; i++) {
        int8_t p = -1;

        if (ota_feed(st, g_img + i, 1, &p) != UPGRADE_PROCESS_OK)
            mono = 0;
    }
    CHECK(mono,
            "every byte before the last is UPGRADE_PROCESS_OK: the first two "
            "bins passing their digests does not complete the image mid-stream");
    rc = ota_feed(st, g_img + len - 1, 1, &pct);
    CHECK(rc == UPGRADE_SUCCEED,
            "...and only the byte that completes the last bin succeeds (rc=%d)", rc);

    ota_reset(NULL, &sk);
    CHECK(1, "ota_reset(NULL, sink) is a no-op rather than a crash");

    free(st);
    printf(fails ? "test_ota_state: FAILED (%d)\n" : "test_ota_state: all pass\n", fails);
    return fails ? 1 : 0;
}
