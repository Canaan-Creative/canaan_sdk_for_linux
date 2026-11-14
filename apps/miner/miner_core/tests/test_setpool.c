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

#include "mc_api_setpool.h"
#include "pool_config.h"
#include "jwrite.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../tests/include/tst_check.h"

#define CONF "test_setpool.conf" /* Pool config file the setpool cases write and update */

static void write_conf(const char *json)
{
    FILE *f = fopen(CONF, "w");
    fprintf(f, "%s", json);
    fclose(f);
}

static int identity_update(pool_set *ps, void *ctx)
{
    (void)ps;
    (void)ctx;
    return 0;
}

static const char *msg_of(mc_setpool_rc rc, const mc_setpool_req *r)
{
    static char m[1280];
    mc_api_setpool_msg(rc, r, m, sizeof m);
    return m;
}

static char *repeat(char *out, char c, int n)
{
    memset(out, c, (size_t)n);
    out[n] = 0;
    return out;
}

int main(void)
{
    mc_setpool_req r;
    char           req[4096], big[512];

    CHECK(mc_api_setpool_parse("setpool|0,stratum+tcp://pool:3333,worker,x", &r) == MC_SETPOOL_OK &&
                    r.argc == 4 && r.index == 0 && !strcmp(r.url, "stratum+tcp://pool:3333") &&
                    !strcmp(r.user, "worker") && !strcmp(r.pass, "x") && !r.clear,
            "4-field parse");
    CHECK(mc_api_setpool_parse("setpool|0,stratum+tcp://pool:3333,worker,x\r\n", &r) ==
                            MC_SETPOOL_OK &&
                    !strcmp(r.pass, "x"),
            "transport line ending stripped");
    CHECK(mc_api_setpool_parse("setpool|1,stratum+tcp://p:3333,u,p", &r) == MC_SETPOOL_OK &&
                    r.index == 1,
            "pool 1 parse");

    CHECK(mc_api_setpool_parse("setpool|admin,pw,0,stratum+tcp://p:3333,u,p", &r) ==
                            MC_SETPOOL_ARGC &&
                    r.argc == 6,
            "the former user name and password fields are not accepted");
    CHECK(mc_api_setpool_parse("setpool|2,stratum+tcp://p:3333,u,", &r) == MC_SETPOOL_ARGC &&
                    r.argc == 3,
            "empty pass -> 3 arguments");
    CHECK(!strcmp(msg_of(MC_SETPOOL_ARGC, &r),
                  "Pool set error: we need 1 or 4 arguments, your input is 3 arguments."),
            "ARGC -> legacy text with the count");
    CHECK(mc_api_setpool_parse("setpool|2,stratum+tcp://p:3333,u,newpw", &r) == MC_SETPOOL_OK &&
                    !strcmp(r.pass, "newpw"),
            "non-empty pass -> change");
    CHECK(mc_api_setpool_parse("setpool|2,stratum+tcp://p:3333,,p", &r) == MC_SETPOOL_ARGC &&
                    r.argc == 2,
            "empty user -> 2 arguments");

    CHECK(mc_api_setpool_parse("setpool|0,,u,p", &r) == MC_SETPOOL_OK && r.clear && r.argc == 1 &&
                    !r.url[0],
            "empty url -> clear");
    CHECK(mc_api_setpool_parse("setpool|1", &r) == MC_SETPOOL_OK && r.clear && r.index == 1 &&
                    r.argc == 1,
            "1-argument form -> clear");

    CHECK(mc_api_setpool_parse("setpool|a,b,c,d", &r) == MC_SETPOOL_ARGC && r.argc == 0,
            "non-numeric pool ends the count at 0");
    CHECK(mc_api_setpool_parse("setpool|0,a,b,c,d", &r) == MC_SETPOOL_ARGC && r.argc == 5,
            "5 arguments -> ARGC");
    CHECK(mc_api_setpool_parse("setpool|0,a", &r) == MC_SETPOOL_ARGC && r.argc == 2,
            "2 arguments -> ARGC");
    CHECK(mc_api_setpool_parse("setpool|0,stra\x01tum,u,p", &r) == MC_SETPOOL_BAD_CHAR,
            "a control byte is refused, never read as the 1-argument clear");
    CHECK(mc_api_setpool_parse("setpool|0,\"x,u,p", &r) == MC_SETPOOL_BAD_CHAR &&
                    !strcmp(msg_of(MC_SETPOOL_BAD_CHAR, &r),
                            "Pool set error: invalid character in arguments"),
            "a quote is refused");
    CHECK(mc_api_setpool_parse("setpool|1x,stratum+tcp://p:3333,u,p", &r) == MC_SETPOOL_ARGC &&
                    r.argc == 0,
            "a malformed pool number is not read as a prefix");

    CHECK(mc_api_setpool_parse("setpool", &r) == MC_SETPOOL_NO_PARAMS, "bare setpool");
    CHECK(mc_api_setpool_parse("setpool|", &r) == MC_SETPOOL_NO_PARAMS, "empty parameter");
    CHECK(mc_api_setpool_parse("setpool|\n", &r) == MC_SETPOOL_NO_PARAMS, "line ending only");
    CHECK(mc_api_setpool_parse("{\"command\":\"setpool\"}", &r) == MC_SETPOOL_NO_PARAMS,
            "JSON without parameter");
    CHECK(!strcmp(msg_of(MC_SETPOOL_NO_PARAMS, &r), "Pool set error: no params"),
            "NO_PARAMS -> legacy text");

    CHECK(mc_api_setpool_parse("{\"command\":\"setpool\",\"parameter\":"
                               "\"2,stratum+tcp://p:3333,u,x\"}",
                  &r) == MC_SETPOOL_OK &&
                    r.index == 2 && !strcmp(r.pass, "x"),
            "JSON parameter");
    CHECK(mc_api_setpool_parse("{\"command\":\"setpool\",\"parameter\":"
                               "\"2,stratum+tcp://p:3333,u,x\\u0000y\"}",
                  &r) == MC_SETPOOL_BAD_CHAR,
            "JSON encoded NUL rejected");

    snprintf(req, sizeof req, "setpool|0,stratum+tcp://%s,u,p",
            repeat(big, 'a', POOL_URL_MAX - 1 - 14));
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_OK && strlen(r.url) == POOL_URL_MAX - 1,
            "256-char url accepted whole");
    snprintf(req, sizeof req, "setpool|0,stratum+tcp://%s,u,p",
            repeat(big, 'a', POOL_URL_MAX - 14));
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_URL_LONG, "257-char url too long");
    CHECK(!strcmp(msg_of(MC_SETPOOL_URL_LONG, &r),
                  "Pool set error: The pool url length is too long."),
            "URL_LONG -> legacy text");
    snprintf(req, sizeof req, "setpool|0,stratum+tcp://p:3333,%s,p",
            repeat(big, 'u', POOL_USER_MAX));
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_USER_LONG, "user too long");
    snprintf(req, sizeof req, "setpool|0,stratum+tcp://p:3333,u,%s",
            repeat(big, 'p', POOL_PASS_MAX));
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_PASS_LONG, "pass too long");

    char longpass[101];
    repeat(longpass, 'q', 100);
    snprintf(req, sizeof req, "setpool|0,stratum+tcp://p:3333,u,%s", longpass);
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_OK && !strcmp(r.pass, longpass),
            "100-char pass accepted + preserved");
    char longuser[101];
    repeat(longuser, 'w', 100);
    snprintf(req, sizeof req, "setpool|0,stratum+tcp://p:3333,%s,x", longuser);
    CHECK(mc_api_setpool_parse(req, &r) == MC_SETPOOL_OK && !strcmp(r.user, longuser),
            "100-char user accepted + preserved");

    CHECK(mc_api_setpool_parse("setpool|5,stratum+tcp://p:3333,u,p", &r) == MC_SETPOOL_OK &&
                    mc_api_setpool_check(&r) == MC_SETPOOL_INDEX,
            "pool 5 parses, then INDEX");
    CHECK(!strcmp(msg_of(MC_SETPOOL_INDEX, &r),
                  "Pool set error: pool num must between 0 and 2 , your input is 5"),
            "INDEX -> legacy text with the value");
    CHECK(mc_api_setpool_parse("setpool|-1,stratum+tcp://p:3333,u,p", &r) == MC_SETPOOL_OK &&
                    mc_api_setpool_check(&r) == MC_SETPOOL_INDEX,
            "pool -1 -> INDEX");
    CHECK(mc_api_setpool_parse("setpool|0,nothost,u,p", &r) == MC_SETPOOL_OK &&
                    mc_api_setpool_check(&r) == MC_SETPOOL_OK && !strcmp(r.url, "nothost"),
            "the url is not validated");
    CHECK(mc_api_setpool_parse("setpool|2", &r) == MC_SETPOOL_OK &&
                    mc_api_setpool_check(&r) == MC_SETPOOL_OK,
            "clear needs no url");

    CHECK(mc_api_setpool_parse("setpool|1,stratum+tcp://p:3333,w.1,x", &r) == MC_SETPOOL_OK,
            "success fixture parses");
    CHECK(!strcmp(msg_of(MC_SETPOOL_OK, &r),
                  "Pool set info: \npool 1 success set to stratum+tcp://p:3333\n"
                  "worker is w.1\nworkerpassword is x\n"
                  "Please reboot miner to make config work.\n"),
            "OK -> legacy info block");
    CHECK(mc_api_setpool_parse("setpool|2", &r) == MC_SETPOOL_OK &&
                    !strcmp(msg_of(MC_SETPOOL_OK, &r),
                            "Pool set info: \npool 2 success set to \nworker is \n"
                            "workerpassword is \nPlease reboot miner to make config work.\n"),
            "clear -> the same block with empty fields");

    pool_set ps;
    pool_init(&ps, 0, 0);
    pool_add(&ps, "stratum+tcp://a:1", "u1", "secret1");
    pool_add(&ps, "stratum+tcp://b:2", "u2", "secret2");
    CHECK(pool_config_apply_slot(&ps, 0, "stratum+tcp://new:1", "u1", "newpw") == 0,
            "overwrite slot 0");
    CHECK(!strcmp(ps.pools[0].url, "stratum+tcp://new:1") && !strcmp(ps.pools[0].pass, "newpw"),
            "url and password replaced");
    CHECK(!strcmp(ps.pools[1].pass, "secret2") && ps.pools[1].slot == 1, "other pool untouched");

    CHECK(pool_config_apply_slot(&ps, 0, "stratum+tcp://new:1", "u1", "") == 0 && ps.count == 1 &&
                    ps.pools[0].slot == 1 && !strcmp(ps.pools[0].url, "stratum+tcp://b:2"),
            "empty pass clears slot 0, slot 1 keeps its slot");

    pool_init(&ps, 0, 0);
    pool_add(&ps, "stratum+tcp://a:1", "u1", "p1");
    pool_config_apply_slot(&ps, 1, "stratum+tcp://b:2", "u2", "realpass");
    CHECK(ps.count == 2 && !strcmp(ps.pools[1].pass, "realpass") && ps.pools[1].slot == 1,
            "append with real pass");

    pool_init(&ps, 0, 0);
    pool_add(&ps, "stratum+tcp://a:1", "u1", "p1");
    CHECK(pool_config_apply_slot(&ps, 2, "stratum+tcp://c:3", "u3", "p3") == 0 && ps.count == 2 &&
                    ps.pools[1].slot == 2,
            "slot 2 written while slot 1 is free");
    CHECK(pool_config_apply_slot(&ps, 1, "stratum+tcp://b:2", "u2", "p2") == 0 && ps.count == 3 &&
                    !strcmp(ps.pools[1].url, "stratum+tcp://b:2") && ps.pools[1].slot == 1 &&
                    ps.pools[2].slot == 2,
            "filling slot 1 inserts it in slot order");

    pool_config_apply_slot(&ps, 1, "", "", "");
    CHECK(ps.count == 2 && !strcmp(ps.pools[0].url, "stratum+tcp://a:1") &&
                    !strcmp(ps.pools[1].url, "stratum+tcp://c:3") && ps.pools[1].slot == 2 &&
                    !strcmp(ps.pools[1].pass, "p3"),
            "clear middle: C stays in slot 2 with its password");

    pool_init(&ps, 0, 0);
    pool_add(&ps, "stratum+tcp://x:1", "ux", "px");
    pool_add(&ps, "stratum+tcp://y:2", "uy", "py");
    pool_add(&ps, "stratum+tcp://z:3", "uz", "pz");
    pool_config_apply_slot(&ps, 0, "stratum+tcp://a:1", "ua", "pa");
    pool_config_apply_slot(&ps, 1, "", "", "");
    pool_config_apply_slot(&ps, 2, "stratum+tcp://c:3", "uc", "pc");
    CHECK(ps.count == 2 && !strcmp(ps.pools[0].url, "stratum+tcp://a:1") && ps.pools[0].slot == 0 &&
                    !strcmp(ps.pools[1].url, "stratum+tcp://c:3") && ps.pools[1].slot == 2,
            "rows A, -, C over X, Y, Z leave no stale pool");

    CHECK(pool_config_apply_slot(&ps, 1, "stratum+tcp://c:3", "other", "x") == 0 && ps.count == 3,
            "duplicate URL is stored at the requested slot");
    repeat(big, 'a', POOL_URL_MAX);
    CHECK(pool_config_apply_slot(&ps, 1, big, "u", "p") == -1 && !strcmp(ps.pools[1].user, "other"),
            "oversized url refused, slot unchanged");
    CHECK(pool_config_apply_slot(&ps, 3, "stratum+tcp://d:4", "u", "p") == -1, "slot 3 refused");

    write_conf("{\n  \"standard\": \"--lowmem\",\n"
               "  \"pools\": {\"pool0\":{\"url\":\"stratum+tcp://x:1\","
               "\"user\":\"u\",\"pass\":\"p\"}}\n}\n");
    pool_config_update(CONF, identity_update, NULL);
    FILE  *f = fopen(CONF, "r");
    char   buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    CHECK(strstr(buf, "\"standard\"") && strstr(buf, "--lowmem"),
            "standard key preserved byte-for-byte");
    CHECK(access(CONF ".tmp", F_OK) != 0, "no .tmp litter");
    CHECK(access(CONF ".lock", F_OK) == 0, ".lock sibling created");

    remove(CONF);
    remove(CONF ".lock");
    remove(CONF ".tmp");
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
