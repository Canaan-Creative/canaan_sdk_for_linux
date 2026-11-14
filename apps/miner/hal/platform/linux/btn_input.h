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

#ifndef BTN_INPUT_H
#define BTN_INPUT_H

#include <stdint.h>

typedef void (*btn_input_cb)(int code, int edge, uint32_t t_ms, void *arg);

int  btn_input_start(btn_input_cb cb, void *arg);
void btn_input_stop(void);

#endif /* BTN_INPUT_H */
