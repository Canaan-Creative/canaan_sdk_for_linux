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

#include "hashmcu.h"
#include "board_tables.h"
#include "boardconf.h"
#include "gpio_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../asic/a3197s/uart_pro.c"

#undef GPIO_ROOT
#define GPIO_ROOT TST_GPIO_ROOT /* sysfs GPIO root overridden to the test's TST_GPIO_ROOT */
#include "../platform/linux/gpio_dev.c"

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            printf("FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } \
    } while (0)
#define SEL_REG   0x42cu                  /* chip register address of the read command frames */
#define SEL_LO(s) ((uint8_t)((s) & 0xff)) /* low 8 bits of chip address s */
#define SEL_HI(s) ((uint8_t)(((s) >> 8) & 0x3)) /* high 2 bits of 10-bit chip address s */

static int               fails;
static const char *const root = GPIO_ROOT;
static int chan_peer[HBOARD_COUNT]; /* test end of the socketpair behind each ASIC UART channel */

static void make_line(int n)
{
    char p[320];
    snprintf(p, sizeof p, "%s/gpio%d", root, n);
    mkdir(p, 0755);
}

static int read_first_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    out[0] = '\0';
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
    return 0;
}

static void expect_out(int n, const char *want, const char *what)
{
    char p[320], got[64];
    snprintf(p, sizeof p, "%s/gpio%d/direction", root, n);
    if (read_first_line(p, got, sizeof got) != 0) {
        CHECK(0, "%s (gpio%d): direction never written — pad not driven at all", what, n);
        return;
    }
    CHECK(strcmp(got, want) == 0, "%s (gpio%d): direction=%s, expected %s", what, n, got, want);
}

static void expect_undriven(int n, const char *what)
{
    char p[320], got[64] = "";
    snprintf(p, sizeof p, "%s/gpio%d/direction", root, n);
    CHECK(read_first_line(p, got, sizeof got) != 0,
            "%s (gpio%d): direction=%s — pad driven that this SKU does not populate", what, n, got);
}

static void bind_channels(void)
{
    for (int i = 0; i < HBOARD_COUNT; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            CHECK(0, "socketpair for channel %d failed", i);
            return;
        }
        uart_hdl[i].fd = sv[0];
        chan_peer[i]   = sv[1];
    }
}

static int wire_read(int ch, uint8_t *buf, size_t cap)
{
    ssize_t n = recv(chan_peer[ch], buf, cap, MSG_DONTWAIT);
    return n < 0 ? 0 : (int)n;
}

static int cmd_frame(int ch, uint8_t *f, size_t cap)
{
    CHECK(uart_write_fifo((uint8_t)ch, SEL_REG, NULL, 1, UART_READ_MODE) == 0,
            "ch%d: uart_write_fifo refused a 1-word read command", ch);
    return wire_read(ch, f, cap);
}

static void check_uart_chip_select(void)
{
    uint8_t f[32];

    bind_channels();

    uart_chip_select(0, 0x123);
    int n = cmd_frame(0, f, sizeof f);
    CHECK(n == 10, "ch0: header-only command is 10 bytes, got %d", n);
    if (n == 10) {
        CHECK(f[0] == 0x5a && f[1] == 0x5a && f[2] == 0x5a && f[3] == 0x5a && f[4] == 0x5a,
                "ch0: HEADER frame is 0x5a x5");
        CHECK(f[5] == 0x23, "ch0: CMD chip-id low byte = 0x23, got 0x%02x", f[5]);
        CHECK(f[6] == (uint8_t)(((SEL_REG & 0x3c) << 2) | (UART_READ_MODE << 2) | 0x1),
                "ch0: CMD addr/mode/chip-id-high byte = 0x%02x, got 0x%02x",
                (uint8_t)(((SEL_REG & 0x3c) << 2) | (UART_READ_MODE << 2) | 0x1), f[6]);
        CHECK(f[7] == (uint8_t)(SEL_REG >> 6), "ch0: CMD addr high byte");
        CHECK(f[8] == 1, "ch0: CMD word count = 1");
        CHECK(f[9] == uart_get_crc8(&f[8], 4), "ch0: CMD CRC-8 over bytes 8..5");
    }

    {
        uint8_t g0[4] = { 0xff, 0xff, 0xff, 0xff };
        uint8_t g1[4] = { 0x55, 0x05, 0x00, 0x1f };
        uint8_t g2[4] = { 0x00, 0x00, 0x11, 0x11 };
        CHECK(uart_get_crc8(&g0[3], 4) == 0x21, "wire CRC golden: ff ff ff ff -> 0x21");
        CHECK(uart_get_crc8(&g1[3], 4) == 0x34, "wire CRC golden: 55 05 00 1f -> 0x34");
        CHECK(uart_get_crc8(&g2[3], 4) == 0x71, "wire CRC golden: 00 00 11 11 -> 0x71");
    }

    if (HBOARD_COUNT > 1) {
        uart_chip_select(0, 0x123);
        uart_chip_select(1, 0x0ab);

        n = cmd_frame(0, f, sizeof f);
        CHECK(n == 10, "ch0: 10 bytes on its own wire, got %d", n);
        if (n == 10)
            CHECK(f[5] == SEL_LO(0x123) && (f[6] & 0x3) == SEL_HI(0x123),
                    "ch0 frame carries chip 0x123, got low 0x%02x high %u — a select on "
                    "another channel overwrote it",
                    f[5], f[6] & 0x3);

        n = cmd_frame(1, f, sizeof f);
        CHECK(n == 10, "ch1: 10 bytes on its own wire, got %d", n);
        if (n == 10)
            CHECK(f[5] == SEL_LO(0x0ab) && (f[6] & 0x3) == SEL_HI(0x0ab),
                    "ch1 frame carries chip 0x0ab, got low 0x%02x high %u", f[5], f[6] & 0x3);

        uart_chip_select(0, 0x001);
        n = cmd_frame(1, f, sizeof f);
        CHECK(n == 10 && f[5] == SEL_LO(0x0ab) && (f[6] & 0x3) == SEL_HI(0x0ab),
                "ch1 selection survives a select on ch0");

        for (int i = 2; i < HBOARD_COUNT; i++)
            CHECK(wire_read(i, f, sizeof f) == 0, "ch%d saw traffic it never sent", i);
    } else {
        uart_chip_select(1, 0x0ab);
        n = cmd_frame(0, f, sizeof f);
        CHECK(n == 10 && f[5] == SEL_LO(0x0ab) && (f[6] & 0x3) == SEL_HI(0x0ab),
                "single-channel board: channel 1 wraps onto channel 0");
    }
}

int main(void)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0 || mkdir(root, 0755) != 0) {
        printf("FAIL: could not build the gpio fixture dir %s\n", root);
        return 1;
    }

    if (CAP_HB_MCU) {
        const int n = (int)HBOARD_COUNT;

        for (size_t i = 0; i < board_hb_mcu_count; i++) {
            make_line(board_hb_mcus[i].rst_gpio);
            make_line(board_hb_mcus[i].bsl_gpio);
        }
        if (I2C_MUX_RESET_GPIO >= 0)
            make_line(I2C_MUX_RESET_GPIO);

        int got = mm_hashboard_bringup();
        CHECK(got == n, "bringup returned %d, expected %d populated board(s)", got, n);

        CHECK(board_hb_mcu_count >= (size_t)HBOARD_COUNT,
                "board_hb_mcus[] has %zu row(s) but HBOARD_COUNT=%d — every hashboard "
                "needs its own MCU released",
                board_hb_mcu_count, n);

        for (int i = 0; i < n; i++) {
            expect_out(board_hb_mcus[i].rst_gpio, "high", "MCU reset released");
            expect_out(board_hb_mcus[i].bsl_gpio, "low", "MCU BSL normal-run");
        }

        for (size_t i = HBOARD_COUNT; i < board_hb_mcu_count; i++) {
            expect_undriven(board_hb_mcus[i].rst_gpio, "unpopulated slot RST");
            expect_undriven(board_hb_mcus[i].bsl_gpio, "unpopulated slot BSL");
        }

        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                CHECK(board_hb_mcus[i].rst_gpio != board_hb_mcus[j].bsl_gpio,
                        "gpio%d is listed as both RST[%d] and BSL[%d]", board_hb_mcus[i].rst_gpio,
                        i, j);

        if (I2C_MUX_RESET_GPIO >= 0) {
            expect_out(I2C_MUX_RESET_GPIO, "high", "i2c mux reset_n released");
            for (int i = 0; i < n; i++) {
                CHECK(I2C_MUX_RESET_GPIO != board_hb_mcus[i].rst_gpio,
                        "mux reset_n collides with RST[%d]", i);
                CHECK(I2C_MUX_RESET_GPIO != board_hb_mcus[i].bsl_gpio,
                        "mux reset_n collides with BSL[%d]", i);
            }
        }

        {
            struct hb_row {
                uint16_t rst, bsl;
            };
            static const struct hb_row ROWS_X3[] = {
                { 40, 41 },
                { 42, 43 },
                { 46, 47 },
            };
            const struct hb_row *want  = NULL;
            size_t               wantn = 0;

            if (!strcmp(BOARDCONF_SECTION, "X3")) {
                want  = ROWS_X3;
                wantn = 3;
            } else {
                CHECK(0,
                        "no expected MCU row list for descriptor [%s] — write it "
                        "by hand off that board's own BOARD_HB_MCU_ROWS",
                        BOARDCONF_SECTION);
            }

            if (want) {
                CHECK(board_hb_mcu_count == wantn,
                        "[%s] declares %zu carrier slot(s) in BOARD_HB_MCU_ROWS, got "
                        "%zu — the slot count is the carrier's, not the SKU's "
                        "HBOARD_COUNT=%d",
                        BOARDCONF_SECTION, wantn, board_hb_mcu_count, n);

                for (size_t i = 0; i < wantn; i++) {
                    if (i >= board_hb_mcu_count) {
                        CHECK(0,
                                "slot %zu is missing from board_hb_mcus[] "
                                "entirely — expected RST %u / BSL %u",
                                i, (unsigned)want[i].rst, (unsigned)want[i].bsl);
                        continue;
                    }
                    CHECK(board_hb_mcus[i].rst_gpio == want[i].rst,
                            "slot %zu RST is gpio%u (got gpio%u) — the pad the release "
                            "pulse drives for that hashboard",
                            i, (unsigned)want[i].rst, (unsigned)board_hb_mcus[i].rst_gpio);
                    CHECK(board_hb_mcus[i].bsl_gpio == want[i].bsl,
                            "slot %zu BSL is gpio%u (got gpio%u) — the pad that must "
                            "stay low or that MCU comes up in its bootstrap loader",
                            i, (unsigned)want[i].bsl, (unsigned)board_hb_mcus[i].bsl_gpio);
                }
            }
        }
    } else {
        CHECK(mm_hashboard_bringup() == 0,
                "a board declaring no MCU pins must report 0 boards brought up");

        CHECK(!strcmp(BOARDCONF_SECTION, "NANO3S"),
                "no expected MCU row list for descriptor [%s] — the only board "
                "declaring CAP_HB_MCU 0 is nano3s",
                BOARDCONF_SECTION);
        CHECK(board_hb_mcu_count == 0,
                "[%s] declares no hashboard MCUs, so the row count is 0 (got %zu) — "
                "every walk of the table is therefore empty and nothing is driven",
                BOARDCONF_SECTION, board_hb_mcu_count);
        for (size_t i = 0; i < (size_t)HBOARD_COUNT; i++) {
            CHECK(board_hb_mcus[i].rst_gpio == 0,
                    "placeholder slot %zu RST is 0 (got gpio%u) — the array is the "
                    "zero-filled stand-in, not a pin table anything may drive",
                    i, (unsigned)board_hb_mcus[i].rst_gpio);
            CHECK(board_hb_mcus[i].bsl_gpio == 0, "placeholder slot %zu BSL is 0 (got gpio%u)", i,
                    (unsigned)board_hb_mcus[i].bsl_gpio);
        }
    }

    check_uart_chip_select();

    printf("%s\n", fails ? "FAILURES" : "all mm_hashboard checks passed");
    return fails ? 1 : 0;
}
