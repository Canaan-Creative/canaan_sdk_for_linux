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

#include "pool_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CONFIG_FIXTURES_DIR
/* Directory holding the config fixtures when the build does not set it */
#define CONFIG_FIXTURES_DIR "."
#endif

#define SAVE_PATH "test_pool_save.conf" /* Pool config file pool_config_save() writes */
#define SAVE_TMP  SAVE_PATH ".tmp"      /* Temporary file pool_config_save() uses for SAVE_PATH */

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    fclose(f);
}

static char *get_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[8192];
    size_t      n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int set_slot2(pool_set *ps, void *ctx)
{
    (void)ctx;
    return pool_config_apply_slot(ps, 2, "stratum+tcp://n:9", "nu", "np");
}

static void test_update(void)
{
    const char *bad =
            "{\"pools\":{\"pool0\":{\"url\":\"stratum+tcp://a:1\",\"user\":\"u\",\"pass\":\"x\"},"
            "\"pool1\":{\"url\":\"stratum+tcp://b:2\",\"user\":\"u\",\"pass\":\"x\"},},"
            "\"other\":1}";
    put_file(SAVE_PATH, bad);
    assert(pool_config_update(SAVE_PATH, set_slot2, NULL) == -1);
    assert(!strcmp(get_file(SAVE_PATH), bad));

    unlink(SAVE_PATH);
    assert(pool_config_update(SAVE_PATH, set_slot2, NULL) == 0);
    pool_set r;
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 1 && r.pools[0].slot == 2);
    unlink(SAVE_PATH);
    unlink(SAVE_PATH ".lock");
}

int main(void)
{
    pool_set ps;
    pool_init(&ps, 1000, 60000);
    int n = pool_config_load(&ps, CONFIG_FIXTURES_DIR "/ref_pools.conf");
    assert(n == 3);
    assert(ps.count == 3);
    assert(!strcmp(ps.pools[0].url, "stratum+tcp://btc.f2pool.com:3333"));
    assert(!strcmp(ps.pools[0].user, "poolworker.001"));
    assert(!strcmp(ps.pools[0].pass, "x"));
    assert(ps.pools[0].alive == 1);

    pool_set e;
    pool_init(&e, 1000, 60000);
    assert(pool_config_load_json(&e, "{\"pools\":{}}") == 0);
    assert(e.count == 0);

    assert(pool_config_load(&e, CONFIG_FIXTURES_DIR "/nope.conf") == -1);

    assert(pool_config_load_json(&e, "{not json") == -1);

    const char *mixed = "{\"pools\":{\"pool0\":{\"url\":\"\",\"user\":\"u\",\"pass\":\"p\"},"
                        "\"pool1\":{\"url\":\"stratum+tcp://h:1\",\"user\":\"w\",\"pass\":\"x\"}}}";
    pool_set    m;
    pool_init(&m, 1000, 60000);
    int mn = pool_config_load_json(&m, mixed);
    assert(mn == 1 && m.count == 1);
    assert(!strcmp(m.pools[0].url, "stratum+tcp://h:1"));
    assert(m.pools[0].slot == 1);

    const char *gap =
            "{\"pools\":{\"pool2\":{\"url\":\"stratum+tcp://c:3\",\"user\":\"w\",\"pass\":\"x\"}}}";
    pool_init(&m, 1000, 60000);
    assert(pool_config_load_json(&m, gap) == 1 && m.pools[0].slot == 2);

    assert(pool_add(&m, "stratum+tcp://d:4", "w", "x") == 1);
    assert(m.pools[0].slot == 0 && m.pools[1].slot == 1);

    (void)chmod(SAVE_PATH, 0600);
    (void)unlink(SAVE_PATH);

    put_file(SAVE_PATH,
            "{\n"
            "  \"pools\": {\"pool0\": {\"url\":\"stratum+tcp://old:1\",\"user\":\"u\","
            "\"pass\":\"p\"}},\n"
            "  \"standard\": \"--lowmem --real-quiet\",\n"
            "  \"queue\": 1,\n"
            "  \"extra\": {\"nested\": [1, 2, {\"pools\": \"decoy\"}]}\n"
            "}");
    pool_set w;
    pool_init(&w, 1000, 60000);
    assert(pool_add(&w, "stratum+tcp://new:3333", "worker.1", "x") == 0);
    assert(pool_add(&w, "stratum+tcp://back:3334", "worker.2", "y") == 1);
    assert(pool_config_save(&w, SAVE_PATH) == 0);

    char *txt = get_file(SAVE_PATH);
    assert(txt);
    assert(strstr(txt, "\"standard\": \"--lowmem --real-quiet\""));
    assert(strstr(txt, "\"queue\": 1"));
    assert(strstr(txt, "\"extra\": {\"nested\": [1, 2, {\"pools\": \"decoy\"}]}"));
    assert(!strstr(txt, "old:1"));
    assert(access(SAVE_TMP, F_OK) != 0);

    pool_set r;
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);
    assert(!strcmp(r.pools[0].url, "stratum+tcp://new:3333"));
    assert(!strcmp(r.pools[0].user, "worker.1"));
    assert(!strcmp(r.pools[0].pass, "x"));
    assert(!strcmp(r.pools[1].url, "stratum+tcp://back:3334"));
    assert(strstr(txt, "\"pool2\":{\"url\":\"\",\"user\":\"\",\"pass\":\"\"}"));

    pool_set h;
    pool_init(&h, 1000, 60000);
    assert(pool_config_load_json(&h,
                   "{\"pools\":{\"pool0\":{\"url\":\"stratum+tcp://a:1\",\"user\":\"u\","
                   "\"pass\":\"x\"},"
                   "\"pool1\":{\"url\":\"\",\"user\":\"\",\"pass\":\"\"},"
                   "\"pool2\":{\"url\":\"stratum+tcp://c:3\",\"user\":\"u\",\"pass\":\"x\"}}}") ==
            2);
    assert(pool_config_save(&h, SAVE_PATH) == 0);
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);
    assert(r.pools[0].slot == 0 && r.pools[1].slot == 2);
    assert(!strcmp(r.pools[1].url, "stratum+tcp://c:3"));

    put_file(SAVE_PATH, "{\"standard\":\"--pools 3 --real-quiet\",\"pools\":{}}");
    assert(pool_config_save(&w, SAVE_PATH) == 0);
    txt = get_file(SAVE_PATH);
    assert(strstr(txt, "\"standard\":\"--pools 3 --real-quiet\""));
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);

    if (geteuid() != 0) {
        put_file(SAVE_PATH, "{\"pools\":{},\"standard\":\"--lowmem\"}");
        assert(chmod(SAVE_PATH, 0444) == 0);
        assert(pool_config_save(&w, SAVE_PATH) == 0);
        pool_init(&r, 1000, 60000);
        assert(pool_config_load(&r, SAVE_PATH) == 2);
        assert(strstr(get_file(SAVE_PATH), "--lowmem"));
    }

    put_file(SAVE_PATH, "{\"standard\":\"--lowmem\"}");
    assert(pool_config_save(&w, SAVE_PATH) == 0);
    txt = get_file(SAVE_PATH);
    assert(!strstr(txt, "standard"));
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);

    put_file(SAVE_PATH, "{not json");
    assert(pool_config_save(&w, SAVE_PATH) == 0);
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);

    unlink(SAVE_PATH);
    assert(pool_config_save(&w, SAVE_PATH) == 0);
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 2);
    assert(access(SAVE_TMP, F_OK) != 0);

    assert(pool_config_save(&w, "/proc/nope/pools.conf") == -1);

    pool_set empty;
    pool_init(&empty, 1000, 60000);
    assert(pool_config_save(&empty, SAVE_PATH) == 0);
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 0);
    unlink(SAVE_PATH);

    const char *withdis =
            "{\"pools\":{"
            "\"pool0\":{\"url\":\"stratum+tcp://a:1\",\"user\":\"u\",\"pass\":\"x\"},"
            "\"pool1\":{\"url\":\"stratum+tcp://b:2\",\"user\":\"u\",\"pass\":\"x\","
            "\"disabled\":true},"
            "\"pool2\":{\"url\":\"stratum+tcp://c:3\",\"user\":\"u\",\"pass\":\"x\"}}}";
    pool_set d;
    pool_init(&d, 1000, 60000);
    assert(pool_config_load_json(&d, withdis) == 3);
    assert(d.pools[0].enabled == 1);
    assert(d.pools[1].enabled == 0);
    assert(d.pools[2].enabled == 1);
    assert(d.pools[1].alive == 1);

    assert(pool_config_save(&d, SAVE_PATH) == 0);
    txt = get_file(SAVE_PATH);
    assert(txt);
    assert(strstr(txt, "\"disabled\":true"));
    pool_init(&r, 1000, 60000);
    assert(pool_config_load(&r, SAVE_PATH) == 3);
    assert(r.pools[0].enabled == 1);
    assert(r.pools[1].enabled == 0);
    assert(r.pools[2].enabled == 1);

    pool_set alle;
    pool_init(&alle, 1000, 60000);
    assert(pool_add(&alle, "stratum+tcp://a:1", "u", "x") == 0);
    assert(pool_add(&alle, "stratum+tcp://b:2", "u", "x") == 1);
    assert(pool_config_save(&alle, SAVE_PATH) == 0);
    txt = get_file(SAVE_PATH);
    assert(txt && !strstr(txt, "disabled"));
    unlink(SAVE_PATH);

    test_update();

    if (geteuid() == 0) {
        printf("SKIP: read-only config is still replaced (root)\n");
        return TST_SKIP;
    }
    printf("test_pool_config: all pass\n");
    return 0;
}
