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

#ifndef MC_ERRCODE_H
#define MC_ERRCODE_H

#define ERR_MM_LAN_DAMAGED   (1u << 1)
#define ERR_MM_POWER         (1u << 2)
#define ERR_MM_FAN           (1u << 3)
#define ERR_MM_IPLOST        (1u << 4)
#define ERR_MM_OVERHOT       (1u << 5)
#define ERR_MM_POOL_INACTIVE (1u << 6)
#define ERR_MM_INTERNUCLEAR  (1u << 7) /* asic link down or never established */
#define ERR_MM_INLETHOT      (1u << 8) /* inlet over temperature */
#define ERR_MM_RBOVERFLOW    (1u << 9) /* ring buffer overflow */
#define ERR_MM_HW_UNMATCH    (1u << 10)
#define ERR_MM_SLAVE         (1u << 11) /* hash board error */
#define ERR_MM_AGING_FAILED  (1u << 13) /* stopped: aging found no usable frequency */

#endif /* MC_ERRCODE_H */
