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

#ifndef MC_API_SETTER_H
#define MC_API_SETTER_H

#include <stddef.h>
#include <stdint.h>

#include "mc_api_types.h"
#include "mc_set_result.h"

#define MC_API_SET_MSG_CAP  576 /* setter Msg buffer, bytes */
#define MC_API_ARGS_LOG_CAP 4096

#define MC_API_SETTER_SECRET 0x1u /* arguments are not logged */
#define MC_API_SETTER_HIDDEN 0x2u /* not listed by help */

/* ascset subcommand handler */
typedef mc_set_result (*mc_api_setter_fn)(uint16_t op, const char *args, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx);

/*
 * Raw setter for a subcommand whose argument is a large blob (wallpaper:
 * mode + signed hex chunk). It gets the unparsed tail after "0,<subcmd>,"
 * (never NULL, NUL-terminated, no control chars) and parses it itself.
 * @asc is the echoed ASC index. Reply rules are those of mc_api_setter_fn.
 */
typedef mc_set_result (*mc_api_setter_raw_fn)(const char *raw_tail, int asc, const mc_api_stats *s,
        char *msg, size_t msgcap, void *ctx);

typedef int (*mc_api_secret_fn)(const char *field, size_t len);
typedef int (*mc_api_reads_fn)(const char *args);

char mc_set_letter(mc_set_result r);
int  mc_set_code(mc_set_result r);
int  mc_api_setter_register(const char *subcmd, uint16_t op, mc_api_setter_fn fn, void *ctx);
mc_api_setter_fn mc_api_setter_lookup(const char *subcmd, void **ctx_out);
int              mc_api_setter_register_raw(const char *subcmd, mc_api_setter_raw_fn fn, void *ctx);
int              mc_api_setter_declare(const char *subcmd, unsigned flags, const char *help_before,
                     mc_api_reads_fn reads);
int              mc_api_setter_always_reads(const char *args);
int mc_api_parse_ascset(const char *req, char *subcmd, size_t scap, char *args, size_t acap,
        int *asc_out);
int mc_api_setter_reads(const char *req);
mc_set_result mc_api_setter_run(const char *req, const mc_api_stats *s, char *msg, size_t msgcap);
void          mc_api_msg_safe(char *msg, size_t msgcap);
void          mc_api_setter_mark_forwarded(void);
void          mc_api_setter_mark_noop(void);
void          mc_api_setter_log_reply(const char *text);
int           mc_api_setter_list(char *buf, size_t cap);
void          mc_api_setter_reset(void);
void          mc_api_setter_set_secret(mc_api_secret_fn fn);
const char   *mc_api_args_redact(const char *args, char *out, size_t cap);

#endif /* MC_API_SETTER_H */
