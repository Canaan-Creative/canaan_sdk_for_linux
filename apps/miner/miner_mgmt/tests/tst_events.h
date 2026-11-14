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

#ifndef TST_EVENTS_H
#define TST_EVENTS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Event log for the supervisor unity tests: stubs record what the loop did
 * with ev(), assertions query the sequence. The including file declares
 * tst_now and the pass counter named by TST_EV_PASS before this header; it
 * may set TST_EV_MAX and TST_EV_STR_MAX (the ev_str() buffer), and defines
 * TST_EV_NO_QUERY to keep its own queries.
 */
#ifndef TST_EV_MAX
#define TST_EV_MAX 512 /* Capacity of tst_ev[], events; later events are counted but not stored */
#endif
#ifndef TST_EV_STR_MAX
#define TST_EV_STR_MAX 640 /* Size of the ev_str() result buffer, bytes */
#endif
#ifndef TST_EV_PASS
#define TST_EV_PASS tst_pass /* Pass counter recorded with each event */
#endif

/* One logged event: stub name, its value, pass counter and tst_now when logged */
typedef struct {
    const char *what;
    int         val;
    int         pass;
    uint64_t    now;
} tst_event;
static tst_event tst_ev[TST_EV_MAX]; /* Event log, in call order */
static int       tst_ev_n;           /* Events logged so far; may exceed TST_EV_MAX */

static inline void ev(const char *what, int val)
{
    if (tst_ev_n < TST_EV_MAX)
        tst_ev[tst_ev_n++] = (tst_event){ what, val, TST_EV_PASS, tst_now };
    else
        tst_ev_n++;
}

#ifndef TST_EV_NO_QUERY
static inline const char *ev_str(int from, int to)
{
    static char b[TST_EV_STR_MAX];
    b[0] = '\0';
    for (int i = from; i < to && i < tst_ev_n; i++) {
        char one[48];
        snprintf(one, sizeof one, "%s%s=%d", i > from ? "," : "", tst_ev[i].what, tst_ev[i].val);
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    return b[0] ? b : "(nothing)";
}

static inline int ev_is(int from, int n, const char *const *want, const int *want_val)
{
    if (from < 0 || from + n > tst_ev_n)
        return 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(tst_ev[from + i].what, want[i]))
            return 0;
        if (want_val && tst_ev[from + i].val != want_val[i])
            return 0;
    }
    return 1;
}

static inline int ev_count(int from, int to, const char *what)
{
    int n = 0;
    for (int i = from; i < to && i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what))
            n++;
    return n;
}

static inline int ev_first(int from, const char *what)
{
    for (int i = from; i < tst_ev_n; i++)
        if (!strcmp(tst_ev[i].what, what))
            return i;
    return -1;
}
#endif /* !TST_EV_NO_QUERY */

#endif /* TST_EVENTS_H */
