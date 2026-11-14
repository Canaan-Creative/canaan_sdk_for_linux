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

#ifndef HASHMCU_H
#define HASHMCU_H

typedef struct {
    int outlet_degc;
    int inlet_degc;
} hashmcu_temps;

int hashmcu_init(void);
int hashmcu_count(void);
int hashmcu_read(int board, hashmcu_temps *out);
int mm_hashboard_bringup(void);

#endif /* HASHMCU_H */
