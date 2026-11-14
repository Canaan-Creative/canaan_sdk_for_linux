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

#ifndef MTD_DEV_H
#define MTD_DEV_H

#include <stddef.h>

int       mtd_dev_init(void);
int       mtd_dev_from_name(const char *name);
long long mtd_dev_size(int mtdnum);
int       mtd_dev_erase(int mtdnum, long long start, long long size);
int       mtd_dev_write(int mtdnum, long long offset, const void *buf, size_t len);
int       mtd_dev_write_end(int mtdnum, int commit);
void      mtd_dev_cleanup(void);

#endif /* MTD_DEV_H */
