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

#ifndef CFGWR_IO_H
#define CFGWR_IO_H

#include <stddef.h>

int cfgwr_read_full(int fd, void *buf, size_t len);
int cfgwr_write_full(int fd, const void *buf, size_t len);

#endif /* CFGWR_IO_H */
