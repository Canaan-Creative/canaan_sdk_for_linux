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

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "osif.h"
#include "boardconf.h"
#include "toast_port.h"
#include "uart_pro.h"
#include "tst_uart.h"
#include "tst_board_expect.h"

#define open tst_uart_open /* routes uart_pro.c UART opens to the test nodes */
#include "../asic/a3197s/uart_pro.c"
#undef open

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            printf("  FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } else { \
            printf("  ok: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

/* Command frame sent by the host: register, UART_READ_MODE or UART_WRITE_MODE, first data word */
struct txrec {
    uint32_t reg;
    int      mode;
    uint32_t word;
};

static int fails;
static int master = -1; /* pty master behind channel 0, -1 = not bound */

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}

static int pty_bind0(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m))
        return -1;
    const char *slave = ptsname(m);
    if (!slave)
        return -1;
    char link[256];
    snprintf(link, sizeof link, "%s/uart%d", TST_UART_ROOT, 0);
    unlink(link);
    if (symlink(slave, link) != 0)
        return -1;
    master = m;
    uart_init(0, UART_DEFAULT_BAUD_RATE);
    return 0;
}

static int frame_reply(uint8_t *out, uint16_t chip, uint32_t reg, const uint32_t *words,
        uint8_t nwords)
{
    memset(out, 0x5a, 5);
    out[5]  = (uint8_t)(chip & 0xff);
    out[6]  = (uint8_t)(((reg & 0x3c) << 2) | ((chip >> 8) & 0x3));
    out[7]  = (uint8_t)(reg >> 6);
    out[8]  = nwords;
    out[9]  = uart_get_crc8(&out[8], 4);
    int off = 10;
    for (uint8_t i = 0; i < nwords; i++) {
        out[off + 0] = (uint8_t)(words[i] >> 0);
        out[off + 1] = (uint8_t)(words[i] >> 8);
        out[off + 2] = (uint8_t)(words[i] >> 16);
        out[off + 3] = (uint8_t)(words[i] >> 24);
        out[off + 4] = uart_get_crc8(&out[off + 3], 4);
        off += 5;
    }
    return off;
}

static void chip_sends(uint16_t chip, uint32_t reg, const uint32_t *words, uint8_t n)
{
    uint8_t f[10 + 5 * 32];
    int     len = frame_reply(f, chip, reg, words, n);
    ssize_t w   = write(master, f, (size_t)len);
    (void)w;
}

static int host_tx(uint8_t *buf, int cap)
{
    int fl = fcntl(master, F_GETFL);
    int n  = 0, r;
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    while (n < cap && (r = (int)read(master, buf + n, (size_t)(cap - n))) > 0)
        n += r;
    fcntl(master, F_SETFL, fl);
    return n;
}

static int parse_host_tx(const uint8_t *b, int len, struct txrec *out, int cap)
{
    int i = 0, n = 0;
    while (i + 10 <= len && n < cap) {
        if (memcmp(b + i, "\x5a\x5a\x5a\x5a\x5a", 5) != 0) {
            i++;
            continue;
        }
        const uint8_t *cmd = b + i + 5;
        out[n].mode        = (cmd[1] >> 2) & 0x3;
        out[n].reg         = ((uint32_t)cmd[2] << 6) | ((cmd[1] & 0xf0) >> 2);
        out[n].word        = 0;
        int words          = (out[n].mode == 1) ? cmd[3] : 0;
        if (words && i + 10 + 5 <= len)
            out[n].word = (uint32_t)cmd[5] | ((uint32_t)cmd[6] << 8) | ((uint32_t)cmd[7] << 16) |
                    ((uint32_t)cmd[8] << 24);
        i += 10 + 5 * words;
        n++;
    }
    return n;
}

static int read_reg(uint32_t reg, uint32_t *v)
{
    uart_write_fifo(0, reg, NULL, 1, UART_READ_MODE);
    return uart_read_fifo(0, reg, v, 1);
}

static int nonce_poll(void)
{
    struct toast_nonce nonce[TOAST_NONCE_COUNT];
    uint32_t           pvmask[8] = { 0 };
    uint8_t            cnt       = 0;
    return uart_get_nonce(0, nonce, &cnt, pvmask);
}

int main(void)
{
    uint8_t      txbuf[4096];
    struct txrec rec[64];
    uint32_t     v = 0, w = 0;

    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", TST_UART_ROOT);
        if (system(cmd) != 0 || mkdir(TST_UART_ROOT, 0755) != 0) {
            printf("  FAIL: could not build the uart fixture dir %s\n", TST_UART_ROOT);
            return 1;
        }
    }

    if (pty_bind0() != 0) {
        printf("  FAIL: could not bind a pty to channel 0\n");
        return 1;
    }
    uart_chip_select(0, 7);
    uart_clear_errcnt(0);

    w = 0x1111;
    chip_sends(4, 0x3704, &w, 1);
    chip_sends(4, 0x3704, &w, 1);
    int rc1 = read_reg(0x3704, &v);
    CHECK(rc1 != 0, "the exchange that met a stale reply fails (rc=%d)", rc1);

    int ntx = parse_host_tx(txbuf, host_tx(txbuf, sizeof txbuf), rec, 64);

    w = 0xabc;
    chip_sends(7, 0x3704, &w, 1);
    int rc2 = read_reg(0x3704, &v);
    CHECK(rc2 == 0 && v == 0xabc,
            "the NEXT exchange succeeds (rc=%d v=0x%x) — the stale queue was "
            "drained, one glitch is one error",
            rc2, v);
    (void)host_tx(txbuf, sizeof txbuf);

    {
        int i = 0;
        while (i < ntx && !(rec[i].mode == 1 && rec[i].reg == 0x3c8c))
            i++;
        CHECK(i + 3 < ntx, "soft reset transmitted (%d frames after the request)", ntx - 1);
        if (i + 3 < ntx) {
            CHECK(rec[i].word == 0x00522001u, "UART CFG3 (0x3c8c) <- 0x00522001 (got 0x%08x)",
                    rec[i].word);
            CHECK(rec[i + 1].mode == 1 && rec[i + 1].reg == 0x200 &&
                            rec[i + 1].word == 0x80000000u && rec[i + 2].mode == 1 &&
                            rec[i + 2].reg == 0x200 && rec[i + 2].word == 0x80000000u,
                    "NONCE_UPDATE (0x200) <- 0x80000000, twice");
            CHECK(rec[i + 3].mode == 1 && rec[i + 3].reg == 0x204 && rec[i + 3].word == 0x800377ffu,
                    "CFG (0x204) <- default 0x800377ff (got 0x%08x)", rec[i + 3].word);
        }
    }

    w = 0x2222;
    chip_sends(4, TOAST_NONCEBUF_ADDR, &w, 1);
    chip_sends(4, TOAST_NONCEBUF_ADDR, &w, 1);
    CHECK(nonce_poll() != 0, "nonce poll meeting a stale reply fails");
    (void)host_tx(txbuf, sizeof txbuf);
    {
        uint32_t note[4] = { 0, 0, 0, 0 };
        chip_sends(7, TOAST_NONCEBUF_ADDR, note, 4);
        CHECK(nonce_poll() == 0, "the NEXT nonce poll succeeds");
        (void)host_tx(txbuf, sizeof txbuf);
    }

    uart_clear_errcnt(0);
    {
        uint32_t junk[5] = { 0, 0, 0, 0, 0 };
        chip_sends(7, TOAST_NONCEBUF_ADDR, junk, 5);
        CHECK(nonce_poll() != 0, "dilen==5 note poll fails");
        int n     = parse_host_tx(txbuf, host_tx(txbuf, sizeof txbuf), rec, 64);
        int found = 0;
        for (int i = 0; i < n; i++)
            if (rec[i].mode == 1 && rec[i].reg == 0x3c8c)
                found = 1;
        CHECK(found, "…and transmitted the soft reset");
        CHECK(uart_get_errcnt(0) == 1, "errcnt counts it once (got %u)", uart_get_errcnt(0));
    }

    uart_clear_errcnt(0);
    w = 0x3333;
    chip_sends(4, 0x3704, &w, 1);
    (void)read_reg(0x3704, &v);
    (void)host_tx(txbuf, sizeof txbuf);
    {
        uint8_t f[15];
        int     len = frame_reply(f, 7, 0x3704, &w, 1);
        f[9] ^= 0xff;
        ssize_t r = write(master, f, (size_t)len);
        (void)r;
        CHECK(read_reg(0x3704, &v) != 0, "CRC-corrupt reply fails the exchange");
        (void)host_tx(txbuf, sizeof txbuf);
        CHECK(uart_get_errcnt(0) == 2,
                "two failed exchanges count 2 (got %u) — no CRC double-count", uart_get_errcnt(0));
    }

    {
        uint32_t           full[TOAST_NONCEBUF_FIFO_LEN + 1];
        struct toast_nonce nonce[TOAST_NONCE_COUNT];
        uint32_t           pvmask[8] = { 0 };
        uint8_t            cnt       = 0;

        for (int i = 0; i < TOAST_NONCEBUF_FIFO_LEN + 1; i++)
            full[i] = 0x01010101u * (uint32_t)(i + 1);
        uart_clear_errcnt(0);
        chip_sends(7, TOAST_NONCEBUF_ADDR, full, TOAST_NONCEBUF_FIFO_LEN);
        int rc = uart_get_nonce(0, nonce, &cnt, pvmask);
        CHECK(rc == 0 && cnt == TOAST_NONCE_COUNT,
                "a full %d-word nonce reply returns %d records (rc=%d cnt=%u)",
                TOAST_NONCEBUF_FIFO_LEN, TOAST_NONCE_COUNT, rc, cnt);
        CHECK(uart_get_errcnt(0) == 0, "...and costs no failed-read count (got %u)",
                uart_get_errcnt(0));
        (void)host_tx(txbuf, sizeof txbuf);

        struct toast_nonce sentinel[TOAST_NONCE_COUNT];
        memset(sentinel, 0xa5, sizeof sentinel);
        memcpy(nonce, sentinel, sizeof nonce);
        cnt = 0xa5;
        uart_clear_errcnt(0);
        chip_sends(7, TOAST_NONCEBUF_ADDR, full, TOAST_NONCEBUF_FIFO_LEN + 1);
        rc = uart_get_nonce(0, nonce, &cnt, pvmask);
        CHECK(rc != 0 && cnt == 0xa5 && memcmp(nonce, sentinel, sizeof nonce) == 0,
                "a %d-word nonce reply is refused before any record is decoded: "
                "neither the count nor the nonce buffer is written (rc=%d cnt=0x%02x)",
                TOAST_NONCEBUF_FIFO_LEN + 1, rc, cnt);
        CHECK(uart_get_errcnt(0) == 1, "...and counts as one failed read (got %u)",
                uart_get_errcnt(0));
        (void)host_tx(txbuf, sizeof txbuf);
    }

    printf("uart per-error recovery / soft reset (%s): %s\n", BOARD_NAME, fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
