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

#ifndef TEMP_OPS_H
#define TEMP_OPS_H

typedef struct hal_temp_ops {
    int (*read)(int *degc); /* reads the board temperature, degC */
} hal_temp_ops_t;

const hal_temp_ops_t *hal_temp(void);

#endif /* TEMP_OPS_H */
