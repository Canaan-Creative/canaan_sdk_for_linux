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

#ifndef MC_SET_RESULT_H
#define MC_SET_RESULT_H

typedef enum {
    MC_SET_OK = 0,    /* applied -> STATUS "S", Code 119, "ASC <n> set OK"  */
    MC_SET_ERR_ARG,   /* missing/invalid argument -> STATUS "E", Code 120   */
    MC_SET_ERR_UNSUP, /* no handler registered for this subcmd -> "E"/120   */
    MC_SET_ERR_HW,    /* handler could not deliver/apply -> "E"/120         */
    /* Appended last on purpose: MC_SET_OK must stay 0 and the error values must
     * keep their numbers, because a result crosses the control channel verbatim
     * in mc_ctrl_ack.result (common/proto/miner_ipc.h). */
    MC_SET_INFO,     /* read-style answer -> "I"/118, "ASC <n> set info: <reason>" */
    MC_SET_ERR_STATE /* refused in the current state -> "E"/120             */
} mc_set_result;

#endif /* MC_SET_RESULT_H */
