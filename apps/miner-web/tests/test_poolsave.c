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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "webform.h"
#include "pool_config.h"

#define CONF "test_poolsave.conf"
#define TMP  CONF ".tmp" /* temporary file of the atomic rewrite */

/* Pool config as stored on the device: one pool plus a non-pool key */
static const char *const DEVICE_CONF = "{\n"
                                       "  \"pools\": {\n"
                                       "    \"pool0\": {\n"
                                       "      \"url\": \"stratum+tcp://btc.f2pool.com:3333\",\n"
                                       "      \"user\": \"olduser\",\n"
                                       "      \"pass\": \"x\"\n"
                                       "    }\n"
                                       "  },\n"
                                       "  \"standard\": \"--lowmem --real-quiet\"\n"
                                       "}";

/* URL-encoded pool form with three pools */
static const char *const FORM =
        "pool1=stratum%2Btcp%3A%2F%2Fbtc.f2pool.com%3A3333&worker1=poolworker.001&passwd1=x&"
        "pool2=stratum%2Btcp%3A%2F%2Fltc.f2pool.com%3A3334&worker2=poolworker.002&passwd2=y&"
        "pool3=stratum%2Btcp%3A%2F%2Fbtc.viabtc.com%3A3335&worker3=poolworker.003&passwd3=z";

static void put_conf(const char *text)
{
    FILE *f = fopen(CONF, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    fclose(f);
}

static char *conf_text(void)
{
    static char buf[8192];
    FILE       *f = fopen(CONF, "rb");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int main(void)
{
    put_conf(DEVICE_CONF);

    pool_set form;
    int      n = web_pools_from_form(FORM, &form);
    assert(n == 3);
    assert(pool_config_save(&form, CONF) == 0);

    pool_set back;
    pool_init(&back, 1000, 60000);
    assert(pool_config_load(&back, CONF) == 3);
    assert(back.count == form.count);
    for (int i = 0; i < form.count; i++) {
        assert(!strcmp(back.pools[i].url, form.pools[i].url));
        assert(!strcmp(back.pools[i].user, form.pools[i].user));
        assert(!strcmp(back.pools[i].pass, form.pools[i].pass));
    }
    assert(!strcmp(back.pools[0].url, "stratum+tcp://btc.f2pool.com:3333"));
    assert(!strcmp(back.pools[2].user, "poolworker.003"));

    const char *txt = conf_text();
    assert(strstr(txt, "\"standard\": \"--lowmem --real-quiet\""));
    assert(!strstr(txt, "olduser"));

    assert(access(TMP, F_OK) != 0);

    pool_set two;
    n = web_pools_from_form("pool1=stratum%2Btcp%3A%2F%2Fa%3A1&worker1=w1&passwd1=p1&"
                            "pool2=&worker2=&passwd2=&"
                            "pool3=stratum%2Btcp%3A%2F%2Fb%3A2&worker3=w3&passwd3=p3",
            &two);
    assert(n == 2);
    assert(pool_config_save(&two, CONF) == 0);
    pool_init(&back, 1000, 60000);
    assert(pool_config_load(&back, CONF) == 2);
    assert(!strcmp(back.pools[0].url, "stratum+tcp://a:1"));
    assert(!strcmp(back.pools[1].url, "stratum+tcp://b:2"));
    assert(back.pools[0].slot == 0 && back.pools[1].slot == 2);
    assert(strstr(conf_text(), "--lowmem --real-quiet"));

    put_conf(DEVICE_CONF);
    assert(pool_config_update(CONF, pool_config_apply_slots, &form) == 0);
    pool_init(&back, 1000, 60000);
    assert(pool_config_load(&back, CONF) == 3);
    for (int i = 0; i < form.count; i++)
        assert(!strcmp(back.pools[i].url, form.pools[i].url));
    assert(strstr(conf_text(), "--lowmem --real-quiet"));
    assert(access(CONF ".lock", F_OK) == 0);
    assert(access(TMP, F_OK) != 0);

    put_conf("{\"pools\":{"
             "\"pool0\":{\"url\":\"stratum+tcp://btc.f2pool.com:3333\",\"user\":\"u\","
             "\"pass\":\"x\",\"disabled\":true},"
             "\"pool1\":{\"url\":\"stratum+tcp://old:1\",\"user\":\"u\","
             "\"pass\":\"x\",\"disabled\":true}}}");
    assert(pool_config_update(CONF, pool_config_apply_slots, &form) == 0);
    pool_init(&back, 1000, 60000);
    assert(pool_config_load(&back, CONF) == 3);
    assert(back.pools[0].slot == 0 && !back.pools[0].enabled);
    assert(!strcmp(back.pools[0].user, "poolworker.001"));
    assert(back.pools[1].slot == 1 && back.pools[1].enabled);
    assert(back.pools[2].slot == 2 && back.pools[2].enabled);

    unlink(CONF);
    unlink(CONF ".lock");
    printf("poolsave: all pass\n");
    return 0;
}
