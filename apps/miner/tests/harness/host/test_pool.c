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

#include "harness_todo.h"
#include "harness_fixture.h"
#include "harness_share.h"
#include "sv1.h"
#include "jread.h"
#include "pool.h"

static void drive_sv1(sv1_client *c, char *jsonl, long want, int *notifies)
{
    sv1_init(c);
    *notifies = 0;
    char *save, *line = strtok_r(jsonl, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        json_value *w = json_parse(line);
        if (!w)
            continue;
        const char *dir   = json_string(json_object_get(w, "dir"));
        long long   conn  = json_int(json_object_get(w, "conn"));
        const char *inner = json_string(json_object_get(w, "line"));
        if (!strcmp(dir, "tx") && conn == want) {
            const json_value *msg = json_object_get(w, "msg");
            const char       *m   = json_string(json_object_get(msg, "method"));
            int               tid = (int)json_int(json_object_get(msg, "id"));
            if (m && !strcmp(m, "mining.configure"))
                c->id_configure = tid;
            if (m && !strcmp(m, "mining.subscribe"))
                c->id_subscribe = tid;
            if (m && !strcmp(m, "mining.authorize"))
                c->id_authorize = tid;
        }
        if (!strcmp(dir, "rx") && conn == want && inner[0]) {
            sv1_event ev;
            sv1_feed_line(c, inner, &ev);
            if (ev.type == SV1_EV_NOTIFY)
                (*notifies)++;
        }
        json_free(w);
    }
}

static void count_shares(const char *path, int *acc, int *ver)
{
    *acc = *ver = 0;
    char *tsv   = hf_read(path);
    if (!tsv)
        return;
    char *sl, *line = strtok_r(tsv, "\n", &sl);
    line = strtok_r(NULL, "\n", &sl);
    while (line) {
        char *c[16] = { 0 };
        int   i     = 0;
        char *s2, *t = strtok_r(line, "\t", &s2);
        while (t && i < 16) {
            c[i++] = t;
            t      = strtok_r(NULL, "\t", &s2);
        }
        if (i >= 16 && !strcmp(c[15], "1")) {
            (*acc)++;
            HsShare s = { .version = c[1],
                .prevhash          = c[2],
                .coinb1            = c[3],
                .coinb2            = c[4],
                .merkle            = c[5],
                .nbits             = c[6],
                .ntime             = c[10],
                .xnonce1           = c[8],
                .xnonce2           = c[9],
                .nonce             = c[11],
                .version_bits      = c[12],
                .mask              = c[13],
                .diff              = c[14] };
            if (hs_meets_target(&s))
                (*ver)++;
        }
        line = strtok_r(NULL, "\n", &sl);
    }
    free(tsv);
}

int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "";

    if (!strcmp(id, "pool_list_max3")) {
        hf_fails    = 0;
        char *pools = hf_read("api/ref_pools.json");
        char *dash  = hf_read("web/get_dashboard.json");
        HF_CHECK(pools && dash, "pools/dashboard fixtures present");
        if (pools && dash) {
            int n = hf_count(pools, "\"URL\":");
            HF_CHECK(n >= 1 && n <= 3, "pools list has %d pool(s), <=3", n);
            HF_CHECK(hf_count(pools, "\"User\":") == n, "every pool carries a User");
            HF_CHECK(hf_count(pools, "\"Password\":") == n, "every pool carries a Password");
            HF_CHECK(hf_count(pools, "\"Status\":\"Alive\"") == n,
                    "every configured pool reports Alive (%d)", n);
            HF_CHECK(hf_count(pools, "\"Status\":\"Dead\"") == 0, "no pool reports Dead");
            HF_CHECK(hf_count(pools, "\"Stratum Active\":true") == 1,
                    "exactly one pool is the active stratum session");
            HF_CHECK(hf_count(pools, "\"Get Failures\":0") == n,
                    "a healthy run reports no Get Failures on any pool");
            HF_CHECK(strstr(dash, "\"pool3\":") != NULL, "dashboard exposes pool3");
            HF_CHECK(strstr(dash, "\"pool4\":") == NULL, "dashboard has no pool4 (max 3)");

            json_value       *pj  = json_parse(pools);
            const json_value *arr = pj ? json_object_get(pj, "POOLS") : NULL;
            pool_set          ps;
            pool_init(&ps, 1000, 60000);
            size_t np = json_array_size(arr), added = 0;
            for (size_t i = 0; i < np; i++) {
                const json_value *e = json_array_get(arr, i);
                if (pool_add(&ps, json_string(json_object_get(e, "URL")),
                            json_string(json_object_get(e, "User")),
                            json_string(json_object_get(e, "Password"))) >= 0)
                    added++;
            }
            HF_CHECK(added == (size_t)n, "pool manager loaded all %d captured pool(s)", n);
            HF_CHECK(ps.count <= POOL_VALID_NUM, "pool manager enforces <=3");
            json_free(pj);
        }
        free(pools);
        free(dash);
        HF_DONE("pool_list_max3");
    }
    if (!strcmp(id, "pool_failover")) {
        hf_fails = 0;
        int p0a, p0v, p1a, p1v;
        count_shares("stratum/failover_pool0.shares.tsv", &p0a, &p0v);
        count_shares("stratum/failover_pool1.shares.tsv", &p1a, &p1v);
        HF_CHECK(p0a >= 1, "primary pool mined (accepted shares: %d)", p0a);
        HF_CHECK(p0v == p0a, "primary shares all hash-verify (%d/%d)", p0v, p0a);
        HF_CHECK(p1a > p0a, "failover: backup carried more work than primary (%d > %d)", p1a, p0a);
        HF_CHECK(p1v >= 10 && p1v * 10 >= p1a * 9,
                "backup resumed real mining: %d/%d shares hash-verify", p1v, p1a);
        HF_DONE("pool_failover");
    }
    if (!strcmp(id, "pool_sv1_session")) {
        hf_fails = 0;
        char *f  = hf_read("stratum/happy.frames");
        HF_CHECK(f != NULL, "transcript frames present");
        if (f) {
            char *cfg = strstr(f, "tx mining.configure");
            char *sub = strstr(f, "tx mining.subscribe");
            char *aut = strstr(f, "tx mining.authorize");
            char *ntf = strstr(f, "rx mining.notify");
            char *sbm = strstr(f, "tx mining.submit");
            HF_CHECK(cfg && sub && aut && ntf && sbm,
                    "configure/subscribe/authorize/notify/submit all present");
            if (sub && aut && ntf && sbm) {
                HF_CHECK(sub < aut, "subscribe precedes authorize");
                HF_CHECK(ntf < sbm, "a job (notify) precedes the first submit");
            }
            HF_CHECK(strstr(f, "rx mining.set_difficulty") != NULL, "set_difficulty received");
            HF_CHECK(hf_count(f, "tx mining.subscribe") >= 2,
                    "reconnect observed (>=2 subscribes)");
            HF_CHECK(hf_count(f, "rx result:true") >= 1, "at least one accepted result");
        }
        free(f);

        char *jl = hf_read("stratum/happy.jsonl");
        HF_CHECK(jl != NULL, "happy.jsonl transcript present");
        if (jl) {
            sv1_client c;
            int        notifies = 0;
            drive_sv1(&c, jl, 1, &notifies);
            HF_CHECK(c.state == SV1_MINING, "SV1 client reached MINING via the transcript");
            HF_CHECK(!strcmp(c.xnonce1, "00") && c.xnonce2_size == 8,
                    "SV1 client negotiated xnonce1/xnonce2_size");
            HF_CHECK(c.version_mask == 0x1fffe000u, "SV1 client negotiated version mask");
            HF_CHECK(notifies >= 1 && c.has_job, "SV1 client parsed a job");
            HF_CHECK(c.untracked >= 1 && c.accepted == 0,
                    "SV1 client accounts no share it did not submit");
            sv1_free(&c);
        }
        free(jl);
        HF_DONE("pool_sv1_session");
    }

    HARNESS_UNKNOWN(id);
}
