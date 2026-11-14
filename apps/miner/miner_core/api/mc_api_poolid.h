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

#ifndef MC_API_POOLID_H
#define MC_API_POOLID_H

typedef enum {
    MC_POOLID_OK = 0, /* id taken with atoi (range unchecked here) */
    MC_POOLID_MISSING /* parameter absent or empty                 */
} mc_poolid_rc;

mc_poolid_rc mc_api_poolid_parse(const char *req, int is_json, int *out_id);

#endif /* MC_API_POOLID_H */
