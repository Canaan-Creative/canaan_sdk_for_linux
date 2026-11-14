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

#ifndef FRAMEWORK_H
#define FRAMEWORK_H

#include <stdint.h>

#define CTL_MAX_RULES 16

typedef struct ctl_rule {
    const char *name;       /* rule name in logs */
    uint32_t    cadence_ms; /* run period, ms */
    void (*evaluate)(void *ctx, uint64_t now_ms);
    void    *ctx;         /* evaluate's first argument */
    uint64_t next_due_ms; /* next run time, ms */
} ctl_rule_t;

int  ctl_register(ctl_rule_t *r);
int  ctl_tick(uint64_t now_ms);
void ctl_reset(void);

#endif /* FRAMEWORK_H */
