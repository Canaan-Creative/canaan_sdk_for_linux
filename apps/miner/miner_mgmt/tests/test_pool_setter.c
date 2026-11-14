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

#include "core_pool.h"
#include "mc_api_cmds.h"
#include "mc_api_command.h"
#include "pool_config.h"
#include "ini.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../tests/include/tst_check.h"

/* Scratch pools.conf path, relative to the working directory */
#define CONF "test_pool_setter_pools.conf"

/* Expected ascset error reply carrying msg */
#define ERR(msg) "STATUS=E,When=0,Code=132,Msg=Pool set error: " msg ",Description="
/* pools.conf JSON with pool0..2 set to hosts x, y and z */
#define XYZ \
    "{\"pools\":{" \
    "\"pool0\":{\"url\":\"stratum+tcp://x:1\",\"user\":\"ux\",\"pass\":\"px\"}," \
    "\"pool1\":{\"url\":\"stratum+tcp://y:2\",\"user\":\"uy\",\"pass\":\"py\"}," \
    "\"pool2\":{\"url\":\"stratum+tcp://z:3\",\"user\":\"uz\",\"pass\":\"pz\"}}}"

void osif_log(int lvl, const char *fmt, ...)
{
    (void)lvl;
    (void)fmt;
}
uint64_t osif_now_ms(void)
{
    return 0;
}

int cfgwr_update_pools(const char *path, pool_config_update_fn update, void *ctx)
{
    return pool_config_update(path, update, ctx);
}

static void write_conf(const char *json)
{
    FILE *f = fopen(CONF, "w");
    fprintf(f, "%s", json);
    fclose(f);
}

static const char *read_conf(void)
{
    static char buf[8192];
    buf[0]  = 0;
    FILE *f = fopen(CONF, "r");
    if (f) {
        buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
        fclose(f);
    }
    return buf;
}

static const char *run_req(const char *req, int is_json)
{
    static char  buf[4096];
    mc_api_stats s = { 0 };
    s.prod         = "Avalon Nano3s";
    s.version      = "v";
    s.dna          = "d";
    s.mac          = "m";
    if (is_json) {
        void         *ctx     = NULL;
        mc_api_cmd_fn handler = mc_api_command_lookup("setpool", &ctx);
        handler(req, &s, 1, buf, sizeof buf, ctx);
    } else {
        mc_api_dispatch_text(req, "setpool", &s, buf, sizeof buf);
    }
    return buf;
}

static const char *run(const char *tail)
{
    char req[2048];
    snprintf(req, sizeof req, "setpool|%s", tail);
    return run_req(req, 0);
}

static const pool_t *slot_at(int slot)
{
    static pool_set ps;
    pool_init(&ps, 0, 0);
    pool_config_load(&ps, CONF);
    for (int i = 0; i < ps.count; i++)
        if (ps.pools[i].slot == slot)
            return &ps.pools[i];
    return NULL;
}

static const char *url_at(int slot)
{
    const pool_t *p = slot_at(slot);
    return p ? p->url : "";
}
static const char *user_at(int slot)
{
    const pool_t *p = slot_at(slot);
    return p ? p->user : "";
}
static const char *pass_at(int slot)
{
    const pool_t *p = slot_at(slot);
    return p ? p->pass : "";
}

static int pool_count(void)
{
    pool_set ps;
    pool_init(&ps, 0, 0);
    int n = pool_config_load(&ps, CONF);
    return n > 0 ? ps.count : 0;
}

int main(void)
{
    write_conf("{\"pools\":{\"pool0\":{\"url\":\"stratum+tcp://a:1\","
               "\"user\":\"u1\",\"pass\":\"secret1\"},"
               "\"pool1\":{\"url\":\"stratum+tcp://b:2\","
               "\"user\":\"u2\",\"pass\":\"secret2\"}}}");

    mc_api_command_reset();
    CHECK(core_pool_setter_register(CONF, NULL) == 0, "setpool registered");

    const char *r = run_req("setpool", 0);
    CHECK(strstr(r, ERR("no params")), "a: no params");
    r = run("0,stratum+tcp://new:1,u1,p1,x,y");
    CHECK(strstr(r, ERR("we need 1 or 4 arguments\\, your input is 6 arguments.")),
            "b: six arguments are refused");
    CHECK(!strcmp(url_at(0), "stratum+tcp://a:1"), "conf untouched on the refused request");
    r = run("5,stratum+tcp://new:1,u1,p1");
    CHECK(strstr(r, ERR("pool num must between 0 and 2 \\, your input is 5")),
            "d: pool out of range, comma escaped in text");
    r = run("0,stratum+tcp://new:1,u1,p=1");
    CHECK(strstr(r,
                  "STATUS=S,When=0,Code=133,Msg=Pool set info: \npool 0 success set to "
                  "stratum+tcp://new:1\nworker is u1\nworkerpassword is p\\=1\n"
                  "Please reboot miner to make config work.\n,Description="),
            "f: success -> the legacy info block");
    CHECK(!strcmp(url_at(0), "stratum+tcp://new:1") && !strcmp(pass_at(0), "p=1"), "url updated");
    CHECK(!strcmp(pass_at(1), "secret2"), "other pool password untouched");
    r = run("1");
    CHECK(strstr(r, "Code=133,Msg=Pool set info: \npool 1 success set to \nworker is \n"),
            "e: 1 argument clears the pool");
    CHECK(!slot_at(1) && !strcmp(url_at(0), "stratum+tcp://new:1"), "slot 1 cleared, slot 0 kept");

    r = run_req("{\"command\":\"setpool\",\"parameter\":"
                "\"5,stratum+tcp://new:1,u1,p1\"}",
            1);
    CHECK(strstr(r, "\"STATUS\":\"E\"") && strstr(r, "\"Code\":132") &&
                    strstr(r,
                            "\"Msg\":\"Pool set error: pool num must between 0 and 2 , "
                            "your input is 5\""),
            "JSON index error");
    r = run_req("{\"command\":\"setpool\",\"parameter\":"
                "\"1,stratum+tcp://b:2,u2,secret2\"}",
            1);
    CHECK(strstr(r, "\"Code\":133") &&
                    strstr(r,
                            "\"Msg\":\"Pool set info: \\u000apool 1 success set to "
                            "stratum+tcp://b:2"),
            "JSON success");

    write_conf(XYZ);
    CHECK(strstr(run("0,stratum+tcp://a:1,ua,pa"), "Code=133"), "row 0 set");
    CHECK(strstr(run("1,,,"), "Code=133"), "row 1 cleared");
    CHECK(strstr(run("2,stratum+tcp://c:3,uc,pc"), "Code=133"), "row 2 set");
    CHECK(pool_count() == 2 && !strcmp(url_at(0), "stratum+tcp://a:1") && !slot_at(1) &&
                    !strcmp(url_at(2), "stratum+tcp://c:3"),
            "A, -, C over X, Y, Z: no stale pool");
    CHECK(strstr(read_conf(), "\"pool1\":{\"url\":\"\",\"user\":\"\",\"pass\":\"\"}"),
            "the free slot is written as empty strings");
    write_conf("{\"pools\":{}}");
    CHECK(strstr(run("2,stratum+tcp://c:3,uc,pc"), "Code=133") && pool_count() == 1 &&
                    !strcmp(url_at(2), "stratum+tcp://c:3"),
            "slot 2 is written while slots 0 and 1 are free");
    write_conf(XYZ);
    CHECK(strstr(run("0,,,"), "Code=133") && pool_count() == 2 &&
                    !strcmp(url_at(1), "stratum+tcp://y:2") &&
                    !strcmp(url_at(2), "stratum+tcp://z:3"),
            "clearing slot 0 moves no other slot");

    char bigreq[1024], big[300];
    memset(big, 'a', 242);
    big[242] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://%s:3333,u,p", big + 5);
    CHECK(strstr(run(bigreq), "Code=133") && strlen(url_at(0)) == 256,
            "256-char url accepted and stored whole");
    memset(big, 'a', 243);
    big[243] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://%s:3333,u,p", big + 5);
    CHECK(strstr(run(bigreq), ERR("The pool url length is too long.")), "257-char url too long");

    char biguser[200];
    memset(biguser, 'u', 199);
    biguser[199] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://p:3333,%s,p", biguser);
    CHECK(strstr(run(bigreq), ERR("The pool user length is too long.")), "user too long");

    char bigpass[POOL_PASS_MAX + 2];
    memset(bigpass, 'p', sizeof bigpass - 1);
    bigpass[sizeof bigpass - 1] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://p:3333,u,%s", bigpass);
    CHECK(strstr(run(bigreq), ERR("The pool pass length is too long.")), "pass too long");

    CHECK(strstr(run("0,a,b,c,d"), ERR("we need 1 or 4 arguments\\, your input is 5 arguments.")),
            "bad argc");

    write_conf("{\"pools\":{\"pool0\":{\"url\":\"stratum+tcp://a:1\","
               "\"user\":\"u1\",\"pass\":\"secret1\"}}}");
    char longpass[101];
    memset(longpass, 'q', 100);
    longpass[100] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://a:1,u1,%s", longpass);
    CHECK(strstr(run(bigreq), "Code=133"), "100-char pass -> success");
    CHECK(!strcmp(pass_at(0), longpass), "100-char pass round-trips intact");
    char longuser[101];
    memset(longuser, 'w', 100);
    longuser[100] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://a:1,%s,x", longuser);
    CHECK(strstr(run(bigreq), "Code=133"), "100-char user -> success");
    CHECK(!strcmp(user_at(0), longuser), "100-char user round-trips intact");

    char boundary[129];
    memset(boundary, 'z', 128);
    boundary[128] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://a:1,%s,%s", boundary, boundary);
    CHECK(strstr(run(bigreq), "Code=133"), "128-byte credentials accepted");
    CHECK(!strcmp(user_at(0), boundary) && !strcmp(pass_at(0), boundary),
            "128-byte credentials preserved on disk");

    CHECK(strstr(run("0,stratum+tcp://a:1,u,"),
                  ERR("we need 1 or 4 arguments\\, your input is 3 arguments.")),
            "empty password -> 3 arguments");
    CHECK(!strcmp(pass_at(0), boundary), "empty password leaves the stored one");
    CHECK(strstr(run("1,stratum+tcp://a:1,other,x"), "Code=133"),
            "same URL with another account accepted");
    CHECK(pool_count() == 2 && !strcmp(user_at(1), "other"), "same-URL slot is actually stored");

    CHECK(strstr(run("0,stratum+tcp://a:1,u,original"), "Code=133"), "seed validation tests");
    const char *bad[] = { ",stratum+tcp://a:1,u,x", "0,stratum+tcp://a:1,u,new\"tail",
        "0,stratum+tcp://a:1,u,new|tail", "0,stratum+tcp://a:1,u,new\ntail",
        "0,stratum+tcp://a:1,,x" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(strstr(run(bad[i]), "STATUS=E,When=0,Code=132"), "invalid input %zu rejected", i);
        CHECK(!strcmp(pass_at(0), "original"), "invalid input preserves config");
    }

    char over[130];
    memset(over, 'z', 129);
    over[129] = 0;
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://a:1,%s,x", over);
    CHECK(strstr(run(bigreq), "STATUS=E"), "129-byte user rejected");
    snprintf(bigreq, sizeof bigreq, "0,stratum+tcp://a:1,u,%s", over);
    CHECK(strstr(run(bigreq), "STATUS=E"), "129-byte pass rejected");
    CHECK(!strcmp(pass_at(0), "original"), "oversize requests leave config unchanged");

    const char *json_requests[] = {
        "{\"command\":\"setpool\",\"parameter\":\"0,stratum+tcp://a:1,u,jsonpass\"}",
        "{\"command\":\"setpool\",\"parameter\":\"0,stratum+tcp://a:1,u,bad\\u0000tail\"}",
        "{\"command\":\"setpool\",\"parameter\":\"0,stratum+tcp://a:1,u,bad\\nline\"}",
        "{\"command\":\"setpool\",\"parameter\":\"0,stratum+tcp://a:1,u,x\"}junk"
    };
    for (size_t i = 0; i < sizeof json_requests / sizeof json_requests[0]; i++) {
        r = run_req(json_requests[i], 1);
        CHECK(strstr(r, i ? "\"STATUS\":\"E\"" : "success set to"),
                "JSON setpool case %zu has expected status", i);
        CHECK(!strcmp(pass_at(0), "jsonpass"), "JSON failure cannot truncate saved password");
    }

    CHECK(strstr(run("1,nothost,u,x"), "Code=133") && !strcmp(url_at(1), "nothost"),
            "an unparseable url is stored unvalidated");

    write_conf("{\"pools\":{\"pool0\":{\"url\":\"stratum+tcp://a:1\","
               "\"user\":\"u1\",\"pass\":\"p1\",\"disabled\":true}}}");
    CHECK(strstr(run("0,stratum+tcp://a:1,u2,p2"), "Code=133") && slot_at(0) &&
                    !slot_at(0)->enabled,
            "same-url setpool keeps a disabled slot disabled");
    CHECK(strstr(run("0,stratum+tcp://new:9,u2,p2"), "Code=133") && slot_at(0) &&
                    slot_at(0)->enabled,
            "new-url setpool re-enables a disabled slot");

    remove(CONF);
    remove(CONF ".lock");
    remove(CONF ".tmp");
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
