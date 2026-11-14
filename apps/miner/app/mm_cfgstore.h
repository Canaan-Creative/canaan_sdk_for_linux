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

#ifndef MM_CFGSTORE_H
#define MM_CFGSTORE_H

#include <stddef.h>
#include <stdint.h>

int  mm_cfgstore_open(const char *path);
int  mm_cfgstore_get(const char *section, const char *key, char *buf, size_t cap);
int  mm_cfgstore_set(const char *section, const char *key, const char *value);
int  mm_cfgstore_set_n(const char *const *sections, const char *const *keys,
         const char *const *values, size_t n);
int  mm_cfgstore_put(const char *section, const char *key, const char *value);
int  mm_cfgstore_commit(const char *section, const char *const *keys, const char *const *values,
         size_t n);
int  mm_cfgstore_flush(void);
int  mm_cfgstore_sync(void);
int  mm_cfgstore_reload(void);
int  mm_cfgstore_fill(const char *defaults_path);
int  mm_cfgstore_recreate(const char *defaults_path);
void mm_cfgstore_freeze(int on);
int  mm_cfgstore_frozen(void);
int  mm_bootby_stage(uint8_t reason, uint32_t sub);
int  mm_bootby_persist(uint8_t reason, uint32_t sub);
void mm_bootby_recover(int wdt_bootstatus, uint8_t *out_reason, uint32_t *out_sub);

#endif /* MM_CFGSTORE_H */
