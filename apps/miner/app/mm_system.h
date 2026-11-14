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

#ifndef MM_SYSTEM_H
#define MM_SYSTEM_H

#include <stdint.h>

int  mm_system_reboot(uint8_t reason, uint32_t sub);
int  mm_system_poweroff(void);
void ota_arm_reboot(void);

#endif /* MM_SYSTEM_H */
