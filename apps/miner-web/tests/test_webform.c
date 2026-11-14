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
#include "webform.h"

int main(void)
{
    char d[64];
    web_url_decode(d, sizeof d, "a%2Bb%3Ac%2Fd+e", 15);
    assert(!strcmp(d, "a+b:c/d e"));

    assert(web_form_get("pool1=x&worker1=w", "worker1", d, sizeof d) && !strcmp(d, "w"));
    assert(!web_form_get("pool1=x", "pool2", d, sizeof d) && d[0] == '\0');

    const char *body =
            "pool1=stratum%2Btcp%3A%2F%2Fbtc.f2pool.com%3A3333&worker1=poolworker.001&passwd1=x&"
            "pool2=stratum%2Btcp%3A%2F%2Fbtc.f2pool.com%3A3333&worker2=poolworker.001&passwd2=x&"
            "pool3=stratum%2Btcp%3A%2F%2Fbtc.f2pool.com%3A3333&worker3=poolworker.001&passwd3=x";
    pool_set ps;
    int      n = web_pools_from_form(body, &ps);
    assert(n == 3);
    assert(!strcmp(ps.pools[0].url, "stratum+tcp://btc.f2pool.com:3333"));
    assert(!strcmp(ps.pools[0].user, "poolworker.001"));
    assert(!strcmp(ps.pools[0].pass, "x"));
    assert(!strcmp(ps.pools[2].url, "stratum+tcp://btc.f2pool.com:3333"));
    assert(ps.pools[0].slot == 0 && ps.pools[1].slot == 1 && ps.pools[2].slot == 2);

    n = web_pools_from_form("pool1=stratum%2Btcp%3A%2F%2Fa%3A1&worker1=w&passwd1=x&pool2=&pool3=",
            &ps);
    assert(n == 1 && !strcmp(ps.pools[0].url, "stratum+tcp://a:1"));

    n = web_pools_from_form("pool1=stratum%2Btcp://a:1&worker1=w&passwd1=x&"
                            "pool2=&worker2=&passwd2=&"
                            "pool3=stratum%2Btcp://c:3&worker3=w&passwd3=x",
            &ps);
    assert(n == 2 && ps.pools[0].slot == 0 && ps.pools[1].slot == 2);
    assert(!strcmp(ps.pools[1].url, "stratum+tcp://c:3"));

    char url[300];
    snprintf(url, sizeof url, "stratum%%2Btcp://%0*d:1", 256 - 14 - 2, 0);
    char form[400];
    snprintf(form, sizeof form, "pool1=%s&worker1=w&passwd1=x", url);
    assert(web_pools_from_form(form, &ps) == 1 && strlen(ps.pools[0].url) == 256);
    snprintf(url, sizeof url, "stratum%%2Btcp://%0*d:1", 256 - 14 - 1, 0);
    snprintf(form, sizeof form, "pool1=%s&worker1=w&passwd1=x", url);
    assert(web_pools_from_form(form, &ps) < 0);

    char longvalue[130];
    memset(longvalue, 'w', 129);
    longvalue[129] = 0;
    char req[1024];
    snprintf(req, sizeof req, "pool1=stratum+tcp://a:1&worker1=%s&passwd1=x", longvalue);
    assert(web_pools_from_form(req, &ps) < 0);
    assert(web_pools_from_form("pool1=stratum%%2Btcp://a:1&worker1=w&passwd1=x", &ps) < 0);
    assert(web_pools_from_form("pool1=stratum%2Btcp://a:1&worker1=w&passwd1=x%00tail", &ps) < 0);
    assert(web_pools_from_form("pool1=stratum%2Btcp://a:1&worker1=w&passwd1=x&pool2=bad&worker2=",
                   &ps) == 1);
    assert(ps.pools[0].slot == 0);
    assert(web_form_get("worker1=a&worker1=b", "worker1", d, sizeof d) < 0);
    assert(web_form_get("worker1=%GG", "worker1", d, sizeof d) < 0);
    assert(web_pools_from_form("pool1=stratum%2Btcp://a:1&worker1=w&passwd1=", &ps) == 1);
    assert(ps.pools[0].slot == 0 && !strcmp(ps.pools[0].user, "w") && ps.pools[0].pass[0] == '\0');
    assert(web_pools_from_form("pool2=stratum%2Btcp://a:1&worker2=w", &ps) == 1);
    assert(ps.pools[0].slot == 1 && ps.pools[0].pass[0] == '\0');
    assert(web_pools_from_form("pool1=stratum%2Btcp://a:1&worker1=&passwd1=x", &ps) == 0);
    printf("webform: all pass\n");
    return 0;
}
