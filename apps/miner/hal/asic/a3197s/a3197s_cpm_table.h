/* SPDX-License-Identifier: MIT */
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

#ifndef A3197S_CPM_TABLE_H
#define A3197S_CPM_TABLE_H

#include <stdint.h>

#define A3197S_CPM_TABLE_COUNT 349

/* {core clock (FPLL/2) in MHz, PLL L1 config word}, ascending frequency */
extern const uint32_t a3197s_cpm_table[A3197S_CPM_TABLE_COUNT][2];

#endif /* A3197S_CPM_TABLE_H */
