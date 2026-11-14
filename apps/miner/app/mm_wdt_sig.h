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

#ifndef MM_WDT_SIG_H
#define MM_WDT_SIG_H

#include <signal.h>
#include <stdint.h>

extern volatile sig_atomic_t g_run; /* 1 until SIGTERM/SIGINT; read by main loop, mm_wdt_thread */
extern volatile sig_atomic_t g_sig; /* signal that cleared g_run, 0 none; read by mm_shutdown() */

void mm_wdt_sig_publish(uint32_t gate);
void mm_wdt_sig_install(void);
void mm_wdt_sig_pretimeout(void);
void mm_wdt_sig_usr2_set(sigset_t *set);

#endif /* MM_WDT_SIG_H */
