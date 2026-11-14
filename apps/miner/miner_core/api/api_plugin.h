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

#ifndef API_PLUGIN_H
#define API_PLUGIN_H

#ifdef API_PLUGIN
#include API_PLUGIN_HEADER
#endif

/*
 * plugin ascset settings: X(name, setter, need_op, flags, help_before, reads), setter
 * an mc_api_setter_fn called with the systemcfg path as ctx; registered only on
 * a board offering need_op, on every board when need_op is CTRL_OP_NONE; flags
 * (MC_API_SETTER_*), help_before (the name help lists it before, NULL: after
 * the built-in names) and reads (an mc_api_reads_fn true for the arguments with
 * which the setting only reads, NULL: it always writes) declared on every board
 */
#ifndef API_PLUGIN_SETTERS
#define API_PLUGIN_SETTERS(X)
#endif

#endif /* API_PLUGIN_H */
