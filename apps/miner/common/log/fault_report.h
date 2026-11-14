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

#ifndef FAULT_REPORT_H
#define FAULT_REPORT_H

#include <stdint.h>

typedef struct fault_report {
    uint32_t code; /* last reported fault code */
} fault_report_t;

typedef enum {
    FAULT_REPORT_NONE = 0, /* code unchanged */
    FAULT_REPORT_CHANGED,  /* code differs from the last reported one */
} fault_report_verdict;

static inline fault_report_verdict fault_report_step(fault_report_t *st, uint32_t code)
{
    if (code == st->code)
        return FAULT_REPORT_NONE;
    st->code = code;
    return FAULT_REPORT_CHANGED;
}

#endif /* FAULT_REPORT_H */
