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

#ifndef NET_UTIL_H
#define NET_UTIL_H

int exec_argv(const char *const argv[], char *res, int res_size);
int exec_argv_timeout(const char *const argv[], char *res, int res_size, int timeout_ms);
int str_to_hex(char *s, int l, char *d, int dcap);
int hex_to_str(char *s, int l, char *d, int dcap);

#endif /* NET_UTIL_H */
