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

#ifndef NET_LOG_H
#define NET_LOG_H

#include "osif.h"

/* Log at a LOG_* level through osif_log() */
#define applog(level, ...) \
    osif_log((level) == LOG_OP             ? OSIF_LOG_OP : \
                    (level) >= LOG_ERR     ? OSIF_LOG_ERR : \
                    (level) >= LOG_WARNING ? OSIF_LOG_WARN : \
                    (level) >= LOG_INFO    ? OSIF_LOG_INFO : \
                                             OSIF_LOG_DBG, \
            __VA_ARGS__)

/* applog() levels; a higher value is more severe */
enum {
    LOG_OP      = 110, /* operator event, OSIF_LOG_OP */
    LOG_ERR     = 100, /* error, OSIF_LOG_ERR */
    LOG_WARNING = 80,  /* warning, OSIF_LOG_WARN */
    LOG_INFO    = 40,  /* information, OSIF_LOG_INFO */
    LOG_DEBUG   = 20,  /* debug, OSIF_LOG_DBG */
};

#endif /* NET_LOG_H */
