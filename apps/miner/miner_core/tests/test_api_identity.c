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

#include "mc_api_cmds.h"
#include "mc_api_estats.h"
#include "mc_api_identity.h"
#include "boardconf.h"

#include "tst_board_expect.h"
#include "tst_stats_json.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) \
    do { \
        if (!(c)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            fails++; \
        } else { \
            printf("  ok: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int fails;

int main(void)
{
    const char *prod, *model, *hwtype, *swtype;

    if (!strcmp(BOARDCONF_SECTION, "NANO3S")) {
        prod   = "Avalon Nano3s";
        model  = "Nano3s";
        hwtype = "N_MM1v1_X1";
        swtype = "MM319";
    } else if (!strcmp(BOARDCONF_SECTION, "X3")) {
        prod   = "Test Board X3";
        model  = "X3";
        hwtype = "X_MM1v1_X2";
        swtype = "MM319";
    } else {
        printf("FAIL: no expected identity for descriptor [%s] — add it here "
               "from the product's own source, never by guessing\n",
                BOARDCONF_SECTION);
        return 1;
    }

    mc_api_stats  s = { 0 };
    mc_api_id_buf ib;
    mc_api_identity_fill(&s, &ib);
    CHECK(s.prod && !strcmp(s.prod, prod), "prod   = \"%s\"", prod);
    CHECK(s.model && !strcmp(s.model, model), "model  = \"%s\"", model);
    CHECK(s.hwtype && !strcmp(s.hwtype, hwtype), "hwtype = \"%s\"", hwtype);
    CHECK(s.swtype && !strcmp(s.swtype, swtype), "swtype = \"%s\"", swtype);

    static char buf[MC_API_REPLY_CAP];
    char        want[256];
    mc_api_version(&s, buf, sizeof buf);
    snprintf(want, sizeof want, "\"PROD\":\"%s\"", prod);
    CHECK(strstr(buf, want), "version JSON %s", want);
    snprintf(want, sizeof want, "\"MODEL\":\"%s\"", model);
    CHECK(strstr(buf, want), "version JSON %s", want);
    snprintf(want, sizeof want, "\"HWTYPE\":\"%s\"", hwtype);
    CHECK(strstr(buf, want), "version JSON %s", want);
    snprintf(want, sizeof want, "\"SWTYPE\":\"%s\"", swtype);
    CHECK(strstr(buf, want), "version JSON %s", want);

    mc_api_version_text(&s, buf, sizeof buf);
    snprintf(want, sizeof want, "PROD=%s,MODEL=%s,HWTYPE=%s,SWTYPE=%s", prod, model, hwtype,
            swtype);
    CHECK(strstr(buf, want), "version text %s", want);

    s.version = "26091101_abcdef0";
    strcpy(s.mm_version, "26091002_1111111");
    snprintf(want, sizeof want,
            "Ver[%s-%s] LVer[26091002_1111111] FW[%s] PVer[" MINER_PROTO_VER "]", model, s.version,
            FW_TYPE);
    mc_api_stats_(&s, buf, sizeof buf, 0, 0);
    CHECK(sj_ok(buf) && sj_has(buf, want) && sj_index(buf, "Ver") == 0 &&
                    sj_in(buf, "Ver", "Sysinfo"),
            "estats JSON opens Sysinfo with Ver %s-%s, PVer right after FW", model, s.version);
    CHECK(!strstr(buf, "26090903_2222222"), "estats JSON carries no asic version");

    snprintf(want, sizeof want,
            "MM ID0=Ver[%s-%s] LVer[26091002_1111111] "
            "FW[%s] PVer[" MINER_PROTO_VER "] ",
            model, s.version, FW_TYPE);
    mc_api_stats_text(&s, buf, sizeof buf, 0, 0);
    CHECK(strstr(buf, want), "estats text opens Ver[%s-%s]", model, s.version);
    CHECK(!strstr(buf, "26090903_2222222"), "estats text carries no asic version");

    mc_api_version(&s, buf, sizeof buf);
    CHECK(strstr(buf, "\"FWVERSION\":\"26091101_abcdef0\""),
            "version JSON carries the build version in FWVERSION");
    CHECK(!strstr(buf, "LVERSION") && !strstr(buf, "BVERSION") && !strstr(buf, "AMVERSION"),
            "version JSON drops the three removed version fields");
    CHECK(!strstr(buf, "1111111") && !strstr(buf, "2222222"),
            "version JSON leaks neither the mm nor the asic component version");
    mc_api_version_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "FWVERSION=26091101_abcdef0,"),
            "version text carries the build version in FWVERSION");
    CHECK(!strstr(buf, "LVERSION") && !strstr(buf, "BVERSION") && !strstr(buf, "AMVERSION"),
            "version text drops the three removed version fields");
    mc_api_litestats_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "LVer[26091002_1111111] FW["),
            "litestats carries the mm version, followed by FW");
    snprintf(want, sizeof want, "LVer[26091002_1111111] FW[%s] PVer[" MINER_PROTO_VER "] STATE[",
            FW_TYPE);
    CHECK(strstr(buf, want), "litestats text carries PVer right after FW");
    mc_api_litestats_(&s, buf, sizeof buf);
    CHECK(sj_ok(buf) && sj_has(buf, want) && sj_type(buf, "PVer") == 's',
            "litestats JSON carries PVer as a string right after FW");

    s.version = "26091598_63395d2t";
    strcpy(s.mm_version, "26091598_63395d2t");
    snprintf(want, sizeof want,
            "Ver[%s-26091598_63395d2t] LVer[26091598_63395d2t] "
            "FW[%s] PVer[" MINER_PROTO_VER "] ",
            model, FW_TYPE);
    mc_api_stats_text(&s, buf, sizeof buf, 0, 0);
    CHECK(strstr(buf, want), "estats text carries the API Ver[] shape");
    mc_api_stats_(&s, buf, sizeof buf, 0, 0);
    CHECK(sj_has(buf, want), "estats JSON carries the API Ver[] shape");
    mc_api_version_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "FWVERSION=26091598_63395d2t,"),
            "version text FWVERSION carries the build commit shape");

    s.mm_version[0] = '\0';
    mc_api_stats_(&s, buf, sizeof buf, 0, 0);
    snprintf(want, sizeof want, "Ver[%s-26091598_63395d2t] LVer[" MINER_VERSION_UNKNOWN "] FW[",
            model);
    CHECK(sj_has(buf, want), "missing versions never fall back to core");
    mc_api_litestats_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "LVer[" MINER_VERSION_UNKNOWN "]"),
            "litestats preserves the unknown mm version");
    mc_api_version_text(&s, buf, sizeof buf);
    CHECK(strstr(buf, "FWVERSION=26091598_63395d2t,"),
            "version FWVERSION stays the build version when mm/asic are unknown");
    printf("%s: %d failure(s) [%s]\n", fails ? "FAIL" : "PASS", fails, BOARDCONF_SECTION);
    return fails ? 1 : 0;
}
