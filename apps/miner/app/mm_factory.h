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

#ifndef MM_FACTORY_H
#define MM_FACTORY_H

#define FACTORY_TIMEZONE "Etc/UTC"

void mm_factory_boot_seed(void);
void mm_factory_boot_fill(void);
void mm_factory_recover(void);
int  mm_factory_in_progress(void);

#endif /* MM_FACTORY_H */
