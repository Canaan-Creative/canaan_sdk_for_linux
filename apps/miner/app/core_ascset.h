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

#ifndef CORE_ASCSET_H
#define CORE_ASCSET_H

#include "osif.h"
#include "webauth.h"

int     core_setters_register(const char *systemcfg_ini);
void    core_setters_ledmode_set(uint8_t mode);
uint8_t core_setters_ledmode(void);
void    core_setters_tz_set(const char *name);

#endif /* CORE_ASCSET_H */
