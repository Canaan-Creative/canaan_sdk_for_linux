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

#include "asic_temp_pub.h"
#include "board_tables.h"
#include "tst_board_expect.h"
#include <stdio.h>

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } \
    } while (0)

#define STATIC_CHECK(cond, tag) typedef char static_assert_##tag[(cond) ? 1 : -1]
STATIC_CHECK(3u * TELEM_PERIOD_MS <= 6000u, telem_period_inside_consumer_window);

STATIC_CHECK(VOL_MIN < VOL_MAX, rail_window_ordered);
STATIC_CHECK(MAX_ASIC_ON_SINGLE_HASH > 0, chain_length_positive);

static int fails;

int main(void)
{
    printf("boardconf section: %s\n", BOARDCONF_SECTION);
    printf("rail window: %d..%d counts, chain=%d, engine slots=%d\n", (int)VOL_MIN, (int)VOL_MAX,
            (int)MAX_ASIC_ON_SINGLE_HASH, (int)MINER_MAX_CHIPS);

    if (TST_BOARD_X3) {
        CHECK(VOL_MIN == 2400 && VOL_MAX == 3000, "expected x3 window 2400..3000, got %d..%d",
                (int)VOL_MIN, (int)VOL_MAX);
        CHECK(VOL_MAX < 3200, "x3 kept a nano3s-era cap (%d)", (int)VOL_MAX);

        CHECK(MAX_ASIC_ON_SINGLE_HASH == 44, "expected a 44-chip chain, got %d",
                (int)MAX_ASIC_ON_SINGLE_HASH);
        CHECK(MAX_ASIC_ON_SINGLE_HASH > 12, "a 12-chip cap would discard a good enum");
        CHECK(HBOARD_COUNT == 2, "expected 2 hashboards, got %d", (int)HBOARD_COUNT);
        CHECK(MINER_MAX_CHIPS == 44, "x3 engine has %d work slots, expected 44",
                (int)MINER_MAX_CHIPS);
        CHECK(MINER_MAX_CHIPS != 12,
                "x3 kept the nano3s 12-slot literal — 32 chips "
                "would enumerate and never receive work");

        {
            static const board_hboard_t hb[] = { BOARD_HBOARD_ROWS };
            _Static_assert(sizeof hb / sizeof hb[0] >= HBOARD_COUNT,
                    "BOARD_HBOARD_ROWS has fewer slots than HBOARD_COUNT populates");
            printf("hashboard slots: %d on the carrier, %d populated\n",
                    (int)(sizeof hb / sizeof hb[0]), (int)HBOARD_COUNT);
            for (unsigned i = 0; i < (unsigned)HBOARD_COUNT; i++) {
                CHECK(hb[i].ri_gpio == 10 + i, "x3 hashboard %u RI expected gpio %u, got %u", i,
                        10 + i, (unsigned)hb[i].ri_gpio);
                CHECK(hb[i].ri_gpio != 41, "gpio41 is HASH0_MCU_BSL, not an RI line");
            }
        }
    } else {
        CHECK(VOL_MIN == 3200 && VOL_MAX == 4000,
                "nano3s window changed: expected 3200..4000, got %d..%d", (int)VOL_MIN,
                (int)VOL_MAX);
        CHECK(MAX_ASIC_ON_SINGLE_HASH == 12, "expected a 12-chip chain, got %d",
                (int)MAX_ASIC_ON_SINGLE_HASH);
        CHECK(MINER_MAX_CHIPS == 12, "nano3s engine has %d work slots, expected 12",
                (int)MINER_MAX_CHIPS);

        {
            static const board_hboard_t hb[] = { BOARD_HBOARD_ROWS };
            _Static_assert(sizeof hb / sizeof hb[0] >= HBOARD_COUNT,
                    "BOARD_HBOARD_ROWS has fewer slots than HBOARD_COUNT populates");
            for (unsigned i = 0; i < (unsigned)HBOARD_COUNT; i++)
                CHECK(hb[i].ri_gpio == 31, "expected nano3s RI gpio 31, got %u",
                        (unsigned)hb[i].ri_gpio);
        }
    }

    CHECK(MINER_MAX_CHIPS == MAX_ASIC_ON_SINGLE_HASH,
            "MINER_MAX_CHIPS %d is not the descriptor's own chain length %d — the "
            "engine would hash only the first %d chip(s)",
            (int)MINER_MAX_CHIPS, (int)MAX_ASIC_ON_SINGLE_HASH, (int)MINER_MAX_CHIPS);

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
