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

#ifndef MM_SPAWN_H
#define MM_SPAWN_H

#include <stdint.h>
#include <sys/types.h>

#define ASIC_START_DELAY_S 15 /* earliest first asic_miner start after mm_miner starts, s */

pid_t spawn_core(void);
pid_t spawn_asic(void);
void  mm_asic_term(pid_t asic);
int   mm_asic_termed(pid_t pid);
void  mm_asic_term_tick(uint64_t now, pid_t asic);
void  mm_start_gate_arm(uint64_t start_ms, pid_t *asic);
void  mm_start_gate_release(uint64_t now, pid_t *asic);
void  mm_spawn_later(int is_core, uint64_t at);
void  mm_spawn_retry(uint64_t now, pid_t *core, pid_t *asic);
int   mm_children_stop(const pid_t *pids, int n, unsigned term_ms, unsigned kill_ms);
void  mm_shutdown(pid_t core, pid_t asic);

#endif /* MM_SPAWN_H */
