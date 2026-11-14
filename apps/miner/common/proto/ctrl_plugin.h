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

#ifndef CTRL_PLUGIN_H
#define CTRL_PLUGIN_H

#ifdef CTRL_PLUGIN
#include CTRL_PLUGIN_HEADER
#endif

#define CTRL_DESCRIBE_LOGGED (-1)

#ifndef CTRL_PLUGIN_CMDS
#define CTRL_PLUGIN_CMDS(X)
#endif
#ifndef CTRL_PLUGIN_PAYLOAD
#define CTRL_PLUGIN_PAYLOAD
#endif
#ifndef CTRL_PLUGIN_BOARD_OPS
#define CTRL_PLUGIN_BOARD_OPS
#endif
#ifndef CTRL_PLUGIN_PARSE
#define CTRL_PLUGIN_PARSE(op, args, out) (-1)
#endif
#ifndef CTRL_PLUGIN_DESCRIBE
#define CTRL_PLUGIN_DESCRIBE(c, buf, cap) 0
#endif
#ifndef CTRL_PLUGIN_VF_TICK
#define CTRL_PLUGIN_VF_TICK()
#endif
#ifndef CTRL_PLUGIN_VF_RESPAWN
#define CTRL_PLUGIN_VF_RESPAWN()
#endif

#endif /* CTRL_PLUGIN_H */
