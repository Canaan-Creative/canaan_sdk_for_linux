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

#include "framework.h"
#include <string.h>

static ctl_rule_t *g_rules[CTL_MAX_RULES]; /* registered rules, in order */
static int         g_count;

int ctl_register(ctl_rule_t *r)
{
    if (!r || !r->name || !r->evaluate)
        return -1;
    if (g_count >= CTL_MAX_RULES)
        return -1;
    r->next_due_ms     = 0;
    g_rules[g_count++] = r;
    return 0;
}

int ctl_tick(uint64_t now_ms)
{
    int ran = 0;
    for (int i = 0; i < g_count; i++) {
        ctl_rule_t *r = g_rules[i];
        if (now_ms < r->next_due_ms)
            continue;
        r->evaluate(r->ctx, now_ms);
        r->next_due_ms = now_ms + r->cadence_ms;
        ran++;
    }
    return ran;
}

void ctl_reset(void)
{
    g_count = 0;
    memset(g_rules, 0, sizeof g_rules);
}
