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

#ifndef MC_API_CMDS_INTERNAL_H
#define MC_API_CMDS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mc_api_types.h"

#define API_VER "3.7"

double      utility(const mc_api_stats *s);
double      dev_utility(const mc_api_stats *s);
double      total_mh(const mc_api_stats *s);
double      dev_mhs_av(const mc_api_stats *s);
void        dev_pcts(const mc_api_stats *s, double *hwp, double *rejp);
const char *dev_status(const mc_api_stats *s);
uint32_t    sum_getworks(const mc_api_stats *s);
const char *coin_lp(const mc_api_stats *s);
void        pool_host(const char *url, char *out, size_t cap);

#endif /* MC_API_CMDS_INTERNAL_H */
