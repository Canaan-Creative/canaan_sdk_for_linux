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

#ifndef AVALON_GEN_H
#define AVALON_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "work.h"
#include "driver_avalon.h"
#include "miner_ipc.h"

#define STATIC_WORK_JOB_ID 0x39240000u /* job id of the built-in default job */

void mc_xnonce2_hex(uint64_t v, int size_bytes, char *out, size_t cap);
int  mc_build_asic_job(const mc_job *j, avalon_job_in *out);
int  mc_gen_chip_work(const mc_asic_job *tpl, uint32_t n2, avalon_job_in *out);
void mc_default_job(mc_asic_job *out);

#endif /* AVALON_GEN_H */
