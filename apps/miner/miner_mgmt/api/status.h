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

#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>

/* One configured pool as dashboard text */
typedef struct {
    const char *url;
    const char *worker;
    const char *passwd;
} mc_pool_view;

/* Dashboard fields as preformatted text, one per JSON key of the same name */
typedef struct {
    const char  *hwtype; /* hardware model name */
    const char  *sys_status;
    const char  *elapsed;     /* run time since core_miner start, s */
    const char  *workingmode; /* work mode index */
    const char  *workingstatus;
    const char  *power;
    const char  *realtime_hash;
    const char  *average_hash;
    const char  *accepted;
    const char  *reject;
    const char  *rejected_percentage;
    const char  *fan_status;
    const char  *fanr; /* fan duty, percent */
    const char  *asic_status;
    const char  *ping;
    const char  *power_status;
    const char  *pool_status;
    const char  *current_pool;
    const char  *address; /* active pool URL */
    const char  *worker;  /* active worker name */
    const char  *mac;     /* MAC address, "xx:xx:xx:xx:xx:xx" */
    const char  *version;
    mc_pool_view pool[3]; /* pools, emitted as pool1..3, worker1..3, passwd1..3 */
} mc_dashboard;

size_t mc_status_dashboard_json(const mc_dashboard *d, char *buf, size_t cap);

#endif /* STATUS_H */
