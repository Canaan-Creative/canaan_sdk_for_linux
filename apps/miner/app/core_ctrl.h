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

#ifndef CORE_CTRL_H
#define CORE_CTRL_H

#include "ctrl_cmds.h"
#include "miner_ipc.h"

#define CORE_CTRL_ACK_MS 2000u /* longest wait for the MSG_CTRL_ACK of a command, ms */

int  core_ctrl_send(mc_ctrl_cmd *cmd, mc_ctrl_ack *ack);
void core_ctrl_ack(const void *data, int len);

#endif /* CORE_CTRL_H */
