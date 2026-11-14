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

#include "status.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

static int fails;

int main(void)
{
    mc_dashboard d = {
        .hwtype              = "Avalon Nano3s",
        .sys_status          = "1",
        .elapsed             = "560",
        .workingmode         = "2",
        .workingstatus       = "1",
        .power               = "132",
        .realtime_hash       = "5.89",
        .average_hash        = "5.66",
        .accepted            = "36",
        .reject              = "0",
        .rejected_percentage = "0",
        .fan_status          = "1",
        .fanr                = "4200",
        .asic_status         = "1",
        .ping                = "1",
        .power_status        = "1",
        .pool_status         = "1",
        .current_pool        = "0",
        .address             = "192.168.1.50",
        .worker              = "poolworker.001",
        .mac                 = "aa:bb:cc:dd:ee:ff",
        .version             = "0.2.0",
        .pool[0]             = { .url = "stratum+tcp://btc.f2pool.com:3333",
                            .worker   = "poolworker.001",
                            .passwd   = "x" },
        .pool[1]             = { .url = "", .worker = "", .passwd = "" },
        .pool[2]             = { .url = "", .worker = "", .passwd = "" },
    };

    char   buf[2048];
    size_t n = mc_status_dashboard_json(&d, buf, sizeof buf);
    CHECK(n < sizeof buf, "assembled within buffer");
    CHECK(buf[0] == '{' && buf[n - 1] == '}', "a plain JSON object");

    static const char *const keys[] = {
        "\"hwtype\":",
        "\"sys_status\":",
        "\"elapsed\":",
        "\"workingmode\":",
        "\"workingstatus\":",
        "\"power\":",
        "\"realtime_hash\":",
        "\"average_hash\":",
        "\"accepted\":",
        "\"reject\":",
        "\"rejected_percentage\":",
        "\"fan_status\":",
        "\"fanr\":",
        "\"asic_status\":",
        "\"ping\":",
        "\"power_status\":",
        "\"pool_status\":",
        "\"current_pool\":",
        "\"address\":",
        "\"worker\":",
        "\"mac\":",
        "\"version\":",
        "\"pool1\":",
        "\"pool2\":",
        "\"pool3\":",
    };
    int all = 1;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strstr(buf, keys[i])) {
            fprintf(stderr, "  missing %s\n", keys[i]);
            all = 0;
        }
    CHECK(all, "all documented telemetry fields present");
    CHECK(strstr(buf, "\"pool1\":\"stratum+tcp://btc.f2pool.com:3333\"") != NULL &&
                    strstr(buf, "\"worker1\":") && strstr(buf, "\"passwd1\":\"x\""),
            "pool1 emitted flat (pool1/worker1/passwd1, capture shape)");

    if (fails) {
        fprintf(stderr, "test_status: %d FAIL\n", fails);
        return 1;
    }
    printf("test_status: all pass\n");
    return 0;
}
