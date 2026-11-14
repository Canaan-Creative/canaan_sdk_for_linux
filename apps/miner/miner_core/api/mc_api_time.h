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

#ifndef MC_API_TIME_H
#define MC_API_TIME_H

#include <stddef.h>

#include "mc_api_setter.h"

mc_set_result mc_api_time_run(const char *req, char *msg, size_t msgcap, int *code);
int           mc_api_time_writes(const char *req);
void          mc_api_time_set_forwarder(mc_set_result (*fn)(const char *tzname));
void          mc_api_time_set_tz_reader(int (*fn)(char *out, size_t cap));

#endif /* MC_API_TIME_H */
