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

#include "a3197s_decode.h"
#include "a3197s_cpm_table.h"
#include "boardconf.h"

uint32_t get_cpm_pll(uint32_t freq)
{
    for (uint32_t i = 0; i < A3197S_CPM_TABLE_COUNT; i++)
        if (freq <= a3197s_cpm_table[i][0])
            return a3197s_cpm_table[i][1];
    return 0;
}

uint32_t get_cpm_freq(uint32_t pll)
{
    pll = (pll & 0x9fffffffu) | 1u;
    for (uint32_t i = 0; i < A3197S_CPM_TABLE_COUNT; i++)
        if (a3197s_cpm_table[i][1] == pll)
            return a3197s_cpm_table[i][0];
    return 0;
}

uint32_t correct_freq(uint32_t freq)
{
    uint32_t pll, fpll;

    if (freq < INIT_FREQ)
        freq = INIT_FREQ;
    for (uint32_t i = 0; i < A3197S_CPM_TABLE_COUNT - 1; i++) {
        pll  = a3197s_cpm_table[i + 1][1];
        fpll = ((pll >> 7) & 0x3ffu) * 24u / (((pll >> 1) & 0x3fu) << ((pll >> 17) & 0x7u));
        if (freq >= a3197s_cpm_table[i][0] && freq < (fpll + 1) / 2)
            return a3197s_cpm_table[i][0];
    }
    return a3197s_cpm_table[A3197S_CPM_TABLE_COUNT - 1][0];
}
