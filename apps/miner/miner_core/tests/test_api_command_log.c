// SPDX-License-Identifier: MIT
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

#include "mc_api_command.h"
#include "osif.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../tests/include/tst_check.h"

static char op_line[512];

static const char *g_reply; /* reply the stub handler returns */

void osif_log(osif_loglvl_t lvl, const char *fmt, ...)
{
    va_list ap;
    if (lvl != OSIF_LOG_OP)
        return;
    va_start(ap, fmt);
    vsnprintf(op_line, sizeof op_line, fmt, ap);
    va_end(ap);
}

static size_t stub_cmd(const char *req, const mc_api_stats *s, int is_json, char *buf, size_t cap,
        void *ctx)
{
    (void)req;
    (void)s;
    (void)is_json;
    (void)ctx;
    return (size_t)snprintf(buf, cap, "%s", g_reply);
}

static void run(const char *reply, int is_json)
{
    char         buf[1024];
    mc_api_stats s;

    memset(&s, 0, sizeof s);
    op_line[0] = '\0';
    g_reply    = reply;
    mc_api_command_run(stub_cmd, "setpool", "", &s, is_json, buf, sizeof buf, NULL);
}

int main(void)
{
    run("STATUS=S,When=1,Code=119,Msg=Pool set info: \npool 0 success set to "
        "stratum+tcp://p:3333\nworker is w.1\nworkerpassword is s3cret\nPlease reboot "
        "miner to make config work.\n,Description=AvalonMiner 1|",
            0);
    CHECK(strstr(op_line, "accepted (code 119") && strstr(op_line, "worker is w.1") &&
                    !strstr(op_line, "s3cret"),
            "text setpool reply is logged without the pool password: %s", op_line);

    run("{\"STATUS\":[{\"STATUS\":\"S\",\"When\":1,\"Code\":119,\"Msg\":\"Pool set "
        "info: \\u000apool 0 success set to stratum+tcp://p:3333\\u000aworker is "
        "w.1\\u000aworkerpassword is s3\\\"cret\\u000aPlease reboot miner to make config "
        "work.\\u000a\",\"Description\":\"AvalonMiner 1\"}]}",
            1);
    CHECK(strstr(op_line, "accepted (code 119") && strstr(op_line, "worker is w.1") &&
                    !strstr(op_line, "s3") && !strstr(op_line, "cret"),
            "json setpool reply is logged without the pool password: %s", op_line);

    run("STATUS=E,When=1,Code=120,Msg=Pool set error: pool num must between 0 and 2 \\, "
        "your input is 5,Description=AvalonMiner 1|",
            0);
    CHECK(strstr(op_line,
                  "refused (code 120: Pool set error: pool num must between 0 and 2 ,"
                  " your input is 5)"),
            "a reply without a password is logged unchanged: %s", op_line);

    return tst_done();
}
