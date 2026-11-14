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

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "telemetry.h"
#include "core_api.h"
#include "api_sock.h"
#include "osif_logpath.h"
#include "mc_errcode.h"
#undef CORE_API_TIMEOUT_MS
#undef MC_API_SOCK_PATH
#undef MINER_WEB_LOG_PATH
#define CORE_API_TIMEOUT_MS 200
#define MC_API_SOCK_PATH    "test_telemetry.sock"
#define MINER_WEB_LOG_PATH  "/dev/null"
#undef _DEFAULT_SOURCE
#include "core_api.c"
#include "telemetry.c"

#define PARSE_ECODES(softoff, echu, ecmm) \
    do { \
        snprintf(blob, sizeof blob, stats_fmt, (softoff), (ecmm), (unsigned)(echu)); \
        mw_telemetry_init(&pt); \
        assert(mw_parse_stats(&pt, blob) == 0); \
    } while (0)

static const char ref_stats[] =
        "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":4133982052,\"Code\":70,"
        "\"Msg\":\"AvalonMiner stats\",\"Description\":\"AvalonMiner 0.3.58\"}],"
        "\"STATS\":[{\"STATS\":0,\"ID\":\"AVALON0\",\"Elapsed\":573,\"Calls\":0,\"Wait\":0.0,"
        "\"Max\":0.0,\"Min\":99999999.0,"
        "\"MMID0Summary\":{\"Sysinfo\":{\"Ver\":\"Nano3s-0.2.0_260703_649070d\","
        "\"LVer\":\"0.2.0_260703_649070d\",\"FW\":\"Release\",\"PVer\":\"1.10\","
        "\"LED\":\"0-1\",\"LEDUser\":\"1-100-50-0-0-255\",\"LcdOnoff\":1,\"LcdSwitch\":1,"
        "\"Volume\":100,\"DNA\":\"0201000012345678\",\"MEMFREE\":49644,\"PFCnt\":0,"
        "\"NETFAIL\":[0,0,0,0,0,0,0,0],\"SYSTEMSTATU\":\"Work: In Work, Hash Board: 1\","
        "\"Elapsed\":596,\"BOOTBY\":\"0x01.00000000\",\"ITemp\":-273,\"OTemp\":68,"
        "\"Fan1\":1460,\"FanR\":31,\"PS\":[0,0,27581,4,0,3574,132],\"Activation\":1,"
        "\"PING\":47,\"POOLS\":0,\"SoftOFF\":0,\"ECMM\":0},\"Hashinfo\":{\"LW\":6693,"
        "\"MH\":[0],\"DHW\":0,\"HW\":0,\"DH\":2.86,\"TMax\":92,\"TAvg\":89,\"TarT\":90,"
        "\"GHSspd\":5892.96,\"DHspd\":2.86,\"GHSmm\":6067.1,\"GHSavg\":5648.81,"
        "\"WU\":78912.97,\"Freq\":415.78,\"MGHS\":[5648.81],\"MTmax\":[92],\"MTavg\":89,"
        "\"TA\":12,\"Core\":\"A3197S\",\"BIN\":26,\"ECHU\":[0],\"PLL0\":[206,0,0,1618],"
        "\"SF0\":[100,415,435,456],\"CRC\":[86],\"COMCRC\":[2],\"ATA2\":\"\",\"WORKMODE\":2,"
        "\"WORKLEVEL\":0,\"MPO\":133,\"CALIALL\":0,\"ADJ\":0,\"BAR\":0}},"
        "\"HBinfo\":{\"HB_0_Info\":{\"PVT_T0\":[87,87,91,90,90,88,90,89,90,88,92,87],"
        "\"PVT_V0\":[287,289,275,284,283,284,278,281,281,285,278,288],\"MW0\":[19,17,16,16,7,"
        "25,16,13,13,16,15,12]}},\"MM Count\":1,\"Nonce Mask\":25},{\"STATS\":1,"
        "\"ID\":\"POOL0\",\"Elapsed\":573,\"Work Diff\":32768.0,\"Times Sent\":40}],\"id\":1}";
static int silent_peer(void)
{
    struct sockaddr_un a  = { .sun_family = AF_UNIX };
    int                fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    strcpy(a.sun_path, MC_API_SOCK_PATH);
    unlink(MC_API_SOCK_PATH);
    assert(bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(fd, 8) == 0);
    return fd;
}

static pid_t serve_once(int lfd, const char *reply, size_t n)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char req[256];
        int  c = accept(lfd, NULL, NULL);
        if (c < 0)
            _exit(1);
        if (read(c, req, sizeof req) <= 0)
            _exit(1);
        for (size_t off = 0; off < n;) {
            ssize_t w = write(c, reply + off, n - off);
            if (w <= 0)
                _exit(1);
            off += (size_t)w;
        }
        close(c);
        _exit(0);
    }
    return pid;
}

static int batch_logged(mw_telemetry *t, const char *json, char *log, size_t cap)
{
    FILE *f = tmpfile();
    assert(f);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0 && dup2(fileno(f), STDERR_FILENO) >= 0);
    int rc = mw_parse_batch(t, json);
    fflush(stderr);
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    rewind(f);
    size_t n = fread(log, 1, cap - 1, f);
    log[n]   = '\0';
    fclose(f);
    return rc;
}

static char *slurp(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/api/%s", API_FIXTURES_DIR, name);
    FILE *f = fopen(path, "rb");
    assert(f && "fixture missing");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(void)
{
    mw_telemetry t;
    mw_telemetry_init(&t);

    char *summary = slurp("ref_summary.json");
    char *pools   = slurp("ref_pools.json");
    char *stats   = strdup(ref_stats);
    char *version = slurp("ref_version.json");
    assert(mw_parse_summary(&t, summary) == 0);
    assert(mw_parse_pools(&t, pools) == 0);
    assert(mw_parse_stats(&t, stats) == 0);
    assert(mw_parse_version(&t, version) == 0);

    const mc_dashboard *d = &t.dash;

    assert(!strcmp(d->elapsed, "565"));
    assert(!strcmp(d->accepted, "37"));
    assert(!strcmp(d->reject, "0"));
    assert(!strcmp(d->rejected_percentage, "0.00"));

    assert(!strcmp(d->current_pool, "1"));
    assert(!strcmp(d->address, "stratum+tcp://btc.f2pool.com:3333"));
    assert(!strcmp(d->worker, "poolworker.001"));
    assert(!strcmp(d->pool[0].url, "stratum+tcp://btc.f2pool.com:3333"));
    assert(!strcmp(d->pool[0].worker, "poolworker.001"));
    assert(d->pool[0].passwd == NULL);
    assert(d->pool[2].url[0] != '\0');

    assert(!strcmp(d->hwtype, "Avalon Nano3s"));
    assert(!strcmp(d->version, "0.2.0_260703_649070d"));
    assert(!strcmp(t.dna, "0201000012345678"));
    assert(!strcmp(d->fan_status, "1460"));
    assert(!strcmp(d->fanr, "31"));
    assert(!strcmp(d->power, "132"));
    assert(!strcmp(d->realtime_hash, "5.89"));
    assert(!strcmp(d->average_hash, "5.65"));
    assert(!strcmp(d->workingmode, "2"));
    assert(!strcmp(d->ping, "47"));
    assert(!strcmp(d->mac, "02:00:00:00:00:01"));

    assert(!strcmp(d->sys_status, "1"));
    assert(!strcmp(d->workingstatus, "1"));
    assert(t.activation == 1);
    assert(!strcmp(d->power_status, "0"));
    assert(!strcmp(d->asic_status, "0"));
    assert(!strcmp(d->pool_status, "1"));

    char   out[4096];
    size_t n = mc_status_dashboard_json(d, out, sizeof out);
    assert(n > 0 && n < sizeof out);
    assert(out[0] == '{');
    assert(strstr(out, "\"realtime_hash\":\"5.89\""));
    assert(strstr(out, "\"version\":\"0.2.0_260703_649070d\""));
    assert(strstr(out, "\"pool1\":\"stratum+tcp://btc.f2pool.com:3333\""));
    assert(strstr(out, "\"worker1\":\"poolworker.001\""));
    assert(strstr(out, "\"passwd1\":\"\"") && strstr(out, "\"passwd3\":\"\""));
    assert(strstr(out, "\"mac\":\"02:00:00:00:00:01\""));

    char worker128[129], pass200[201], json[1024];
    memset(worker128, 'w', 128);
    worker128[128] = 0;
    memset(pass200, 'p', 200);
    pass200[200] = 0;
    snprintf(json, sizeof json,
            "{\"POOLS\":[{\"URL\":\"stratum+tcp://a:1\",\"User\":\"%s\",\"Password\":\"%s\"}]}",
            worker128, pass200);
    assert(mw_parse_pools(&t, json) == 0);
    assert(!strcmp(t.pool_worker[0], worker128));
    n = mc_status_dashboard_json(d, out, sizeof out);
    assert(n > 0 && n < sizeof out && !strstr(out, pass200));
    assert(!t.pool_url[1][0] && !t.pool_url[2][0]);

    /* No pool configured: the core answers the "No pools" (Code 8) error. That
     * is a valid empty-pool state, not a parse failure, so the pool card reads
     * Inactive and address/worker show "no active pool". */
    static const char *no_pools =
            "{\"STATUS\":[{\"STATUS\":\"E\",\"When\":1,\"Code\":8,\"Msg\":\"No pools\","
            "\"Description\":\"AvalonMiner 0.3.58\"}],\"id\":1}";
    mw_telemetry np;
    mw_telemetry_init(&np);
    assert(mw_parse_pools(&np, no_pools) == 0);
    assert(!strcmp(np.dash.pool_status, "0"));
    assert(!strcmp(np.dash.address, "no active pool"));
    assert(!strcmp(np.dash.worker, "no active pool"));
    assert(np.dash.current_pool[0] == '\0');

    /* A pool is configured but none is stratum-active: same no-active-pool face. */
    mw_telemetry_init(&np);
    assert(mw_parse_pools(&np,
                   "{\"POOLS\":[{\"URL\":\"stratum+tcp://a:1\",\"User\":\"w\",\"Password\":\"x\","
                   "\"Stratum Active\":false}],\"id\":1}") == 0);
    assert(!strcmp(np.dash.pool_status, "0"));
    assert(!strcmp(np.dash.address, "no active pool"));
    assert(!strcmp(np.dash.worker, "no active pool"));

    /* A different error with no POOLS array is still a genuine parse failure. */
    mw_telemetry_init(&np);
    assert(mw_parse_pools(&np,
                   "{\"STATUS\":[{\"STATUS\":\"E\",\"Code\":14,"
                   "\"Msg\":\"Invalid command\"}],\"id\":1}") == -1);

    /* The same "No pools" reply inside a '+'-batch is accepted, not rejected. */
    char nbatch[512];
    snprintf(nbatch, sizeof nbatch,
            "{\"summary\":{\"SUMMARY\":[{\"Elapsed\":5}]},\"pools\":%s,"
            "\"stats\":{\"STATS\":[{}]},\"version\":{\"VERSION\":[{}]}}",
            no_pools);
    char log[1024];
    mw_telemetry_init(&np);
    assert(batch_logged(&np, nbatch, log, sizeof log) == 0);
    assert(log[0] == '\0');
    assert(!strcmp(np.dash.pool_status, "0"));
    assert(!strcmp(np.dash.address, "no active pool"));

    static const char *const bad_pools[] = {
        "{\"POOLS\":[{\"URL\":\"a\"},{\"URL\":\"b\"},{\"URL\":\"c\"},{\"URL\":\"d\"}],\"id\":1}",
        "{\"STATUS\":[{\"STATUS\":\"E\",\"Code\":14,\"Msg\":\"Invalid command\"}],\"id\":1}",
    };
    for (size_t i = 0; i < sizeof bad_pools / sizeof bad_pools[0]; i++) {
        assert(mw_parse_pools(&np, bad_pools[i]) == -1);
        snprintf(nbatch, sizeof nbatch,
                "{\"summary\":{\"SUMMARY\":[{\"Elapsed\":5}]},\"pools\":%s,"
                "\"stats\":{\"STATS\":[{}]},\"version\":{\"VERSION\":[{\"MAC\":"
                "\"020000000001\"}]}}",
                bad_pools[i]);
        mw_telemetry_init(&np);
        assert(batch_logged(&np, nbatch, log, sizeof log) == 0);
        assert(log[0] == '\0');
        assert(!strcmp(np.dash.elapsed, "5"));
        assert(!strcmp(np.dash.mac, "02:00:00:00:00:01"));
        assert(np.dash.pool_status[0] == '\0' && np.dash.pool[0].url[0] == '\0');
    }

    mw_telemetry vt;
    mw_telemetry_init(&vt);
    assert(mw_parse_version(&vt, "{\"VERSION\":[{\"MAC\":\"000000000000\"}],\"id\":1}") == 0);
    assert(!strcmp(vt.mac, "00:00:00:00:00:00"));
    mw_telemetry_init(&vt);
    assert(mw_parse_version(&vt, "{\"VERSION\":[{\"DNA\":\"0201\"}],\"id\":1}") == 0);
    assert(vt.mac[0] == '\0');
    assert(mw_parse_version(&vt, "not json") == -1);

    char *batch = malloc(strlen(summary) + strlen(pools) + strlen(stats) + strlen(version) + 64);
    assert(batch);
    sprintf(batch, "{\"summary\":%s,\"pools\":%s,\"stats\":%s,\"version\":%s,\"id\":1}", summary,
            pools, stats, version);
    mw_telemetry bt;
    mw_telemetry_init(&bt);
    assert(mw_parse_batch(&bt, batch) == 0);
    assert(!strcmp(bt.dash.elapsed, "565"));
    assert(!strcmp(bt.dash.realtime_hash, "5.89"));
    assert(!strcmp(bt.dash.hwtype, "Avalon Nano3s"));
    assert(!strcmp(bt.dash.mac, "02:00:00:00:00:01"));
    assert(!strcmp(bt.dash.pool[0].url, "stratum+tcp://btc.f2pool.com:3333"));
    free(batch);

    assert(mw_parse_batch(&bt, "{\"STATUS\":[{\"Msg\":\"Invalid command\"}],\"id\":1}") == -1);
    assert(mw_parse_batch(&bt, "not json") == -1);

    free(summary);
    free(pools);
    free(stats);
    free(version);

    static const char *layered_stats =
            "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":1000,\"Code\":70,"
            "\"Msg\":\"AvalonMiner stats\",\"Description\":\"AvalonMiner 0.3.58\"}],"
            "\"STATS\":[{\"STATS\":0,\"ID\":\"AVALON0\",\"Elapsed\":565,\"Calls\":0,\"Wait\":0.0,"
            "\"Max\":0.0,\"Min\":99999999.0,"
            "\"MMID0Summary\":{\"Sysinfo\":{\"Ver\":\"Nano3s-26091702_1111111\","
            "\"LVer\":\"26091701_2222222\",\"FW\":\"Release\",\"PVer\":\"1.10\",\"LED\":\"0-1\","
            "\"LcdOnoff\":1,\"LcdSwitch\":1,\"Volume\":100,\"DNA\":\"0201000012345678\","
            "\"MEMFREE\":0,\"PFCnt\":0,\"NETFAIL\":[0,0,0,0,0,0,0,0],"
            "\"SYSTEMSTATU\":\"Work: In Work, Hash Board: 1\",\"Elapsed\":565,"
            "\"BOOTBY\":\"0x01.00000000\",\"ITemp\":-273,\"OTemp\":41,\"Fan1\":0,\"FanR\":0,"
            "\"PS\":[0,0,27581,4,0,3314,112],\"Activation\":1,\"PING\":47,\"POOLS\":0,"
            "\"SoftOFF\":0,\"ECMM\":0},\"Hashinfo\":{\"LW\":120,\"MH\":[0],\"DHW\":0,\"HW\":0,"
            "\"DH\":0.0,\"TMax\":58,\"TAvg\":52,\"TarT\":0,\"GHSspd\":1459.2,\"DHspd\":0.0,"
            "\"GHSmm\":1459.2,\"GHSavg\":1459.2,\"WU\":71335.65,\"Freq\":100.0,\"MGHS\":[1459.2],"
            "\"MTmax\":[58],\"MTavg\":52,\"TA\":12,\"Core\":\"A3197S\",\"BIN\":0,\"ECHU\":[0],"
            "\"WORKMODE\":2,\"WORKLEVEL\":0,\"MPO\":0,\"CALIALL\":0,\"ADJ\":0,\"BAR\":0}},"
            "\"HBinfo\":{\"HB_0_Info\":{\"PVT_T0\":[],\"PVT_V0\":[],\"MW0\":[]}},\"MM Count\":1,"
            "\"Nonce Mask\":25}],\"id\":1}";
    mw_telemetry lt;
    mw_telemetry_init(&lt);
    assert(mw_parse_stats(&lt, layered_stats) == 0);
    const mc_dashboard *ld = &lt.dash;
    assert(!strcmp(ld->hwtype, "Avalon Nano3s"));
    assert(!strcmp(ld->version, "26091701_2222222"));
    assert(!strcmp(ld->power, "112"));
    assert(!strcmp(ld->realtime_hash, "1.46"));
    assert(!strcmp(ld->average_hash, "1.46"));
    assert(!strcmp(ld->workingmode, "2"));
    assert(!strcmp(ld->ping, "47"));
    assert(!strcmp(ld->sys_status, "1"));
    assert(!strcmp(ld->workingstatus, "1"));
    assert(lt.activation == 1);
    assert(!strcmp(ld->power_status, "0"));
    assert(!strcmp(ld->asic_status, "0"));
    assert(!strcmp(ld->fan_status, "0"));
    assert(!strcmp(ld->fanr, "0"));

    static const char *x3 =
            "{\"STATUS\":[{\"STATUS\":\"S\",\"When\":1787524420,\"Code\":70}],"
            "\"STATS\":[{\"STATS\":0,\"ID\":\"AVALON0\",\"Elapsed\":3,\"Calls\":0,\"Wait\":0.0,"
            "\"Max\":0.0,\"Min\":99999999.0,"
            "\"MMID0Summary\":{\"Sysinfo\":{\"Ver\":\"X3-26071690_434669a\","
            "\"LVer\":\"26071690_434669a\",\"FW\":\"Debug\",\"PVer\":\"0.5\","
            "\"DNA\":\"0201000012345678\",\"MEMFREE\":0,\"PFCnt\":0,\"NETFAIL\":[0,0,0,0,0,0,0,0],"
            "\"SYSTEMSTATU\":\"Work: In Work, Hash Board: 2\",\"Elapsed\":14,"
            "\"BOOTBY\":\"0x03.00000000\",\"ITemp\":18,\"OTemp\":40,\"Fan1\":5820,\"Fan2\":5670,"
            "\"Fan3\":5710,\"FanR\":100,\"PS\":[0,0,2900,12010,0,2900,279],\"PING\":0,\"POOLS\":0,"
            "\"SoftOFF\":0,\"ECMM\":0},\"Hashinfo\":{\"LW\":0,\"MH\":[0,0],\"DHW\":0,\"HW\":0,"
            "\"DH\":0.0,\"HBITemp\":[49,51],\"HBOTemp\":[53,55],\"TMax\":0,\"TAvg\":0,\"TarT\":80,"
            "\"GHSspd\":0.0,\"DHspd\":0.0,\"GHSmm\":0.0,\"GHSavg\":0.0,\"WU\":0.0,\"Freq\":0.0,"
            "\"MGHS\":[0.0,0.0],\"MTmax\":[0,0],\"MTavg\":0,\"TA\":0,\"Core\":\"A3197S\","
            "\"BIN\":0,\"ECHU\":[0,0],\"PLL0\":[0,0,0,0],\"SF0\":[0,0,0,0],\"PLL1\":[0,0,0,0],"
            "\"SF1\":[0,0,0,0],\"CRC\":[0,0],\"COMCRC\":[0,0],\"ATA0\":\"300-85-2900-350-25\","
            "\"WORKMODE\":2,\"WORKLEVEL\":0,\"MPO\":300,\"CALIALL\":1,\"ADJ\":1}},"
            "\"HBinfo\":{\"HB_0_Info\":{\"PVT_T0\":[],\"PVT_V0\":[],\"MW0\":[]},"
            "\"HB_1_Info\":{\"PVT_T1\":[],\"PVT_V1\":[],\"MW1\":[]}},\"MM Count\":1,"
            "\"Nonce Mask\":0}],\"id\":1}";
    mw_telemetry at;
    mw_telemetry_init(&at);
    assert(mw_parse_stats(&at, x3) == 0);
    const mc_dashboard *ad = &at.dash;

    assert(!strcmp(ad->version, "26071690_434669a"));
    assert(!strcmp(ad->hwtype, "Avalon X3"));
    assert(!strcmp(at.dna, "0201000012345678"));
    assert(!strcmp(ad->fan_status, "5820"));
    assert(!strcmp(ad->fanr, "100"));
    assert(!strcmp(ad->power, "279"));
    assert(!strcmp(ad->realtime_hash, "0.00"));
    assert(!strcmp(ad->average_hash, "0.00"));
    assert(!strcmp(ad->workingmode, "2"));
    assert(!strcmp(ad->ping, "0"));
    assert(!strcmp(ad->sys_status, "1"));
    assert(!strcmp(ad->power_status, "0"));
    assert(!strcmp(ad->asic_status, "0"));
    assert(ad->workingstatus[0] == '\0');
    assert(at.activation == 0);

    mw_telemetry et;
    mw_telemetry_init(&et);
    assert(mw_parse_stats(&et,
                   "{\"STATS\":[{\"MMID0Summary\":{\"Sysinfo\":{\"FW\":\"x LVer[1] PING[9]\","
                   "\"Ver\":\"Nano3s-26102001_1530_d72edcat-485f064t\","
                   "\"LVer\":\"26102001_1530_d72edcat-485f064t\",\"PING\":\"47\"}}}],"
                   "\"id\":1}") == 0);
    assert(!strcmp(et.dash.version, "26102001_1530_d72edcat-485f064t"));
    assert(!strcmp(et.dash.hwtype, "Avalon Nano3s"));
    assert(et.dash.ping[0] == '\0');

    /* the former "MM ID0" string carries no fields */
    mw_telemetry_init(&et);
    assert(mw_parse_stats(&et,
                   "{\"STATS\":[{\"MM ID0\":\"Ver[Nano3s-1] LVer[2] ECMM[1]\"}],\"id\":1}") == 0);
    assert(et.dash.hwtype[0] == '\0' && et.dash.version[0] == '\0');
    assert(et.dash.asic_status[0] == '\0');

    static const char *const stats_fmt =
            "{\"STATS\":[{\"STATS\":0,\"ID\":\"AVALON0\",\"MMID0Summary\":{\"Sysinfo\":{"
            "\"Ver\":\"Nano3s-26091702_1111111\",\"LVer\":\"26091701_2222222\",\"SoftOFF\":%d,"
            "\"ECMM\":%u},\"Hashinfo\":{\"ECHU\":[%u]}}}],\"id\":1}";
    char         blob[512];
    mw_telemetry pt;

    PARSE_ECODES(3, 0, 0u);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "0"));

    PARSE_ECODES(5, 0, ERR_MM_FAN);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    PARSE_ECODES(1, 0, ERR_MM_POWER);
    assert(!strcmp(pt.dash.power_status, "1"));

    PARSE_ECODES(0, 0, ERR_MM_POWER);
    assert(!strcmp(pt.dash.power_status, "1"));

    PARSE_ECODES(0, 0, ERR_MM_OVERHOT);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    PARSE_ECODES(0, 1, 0u);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    PARSE_ECODES(0, 4294967295u, 0u);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    PARSE_ECODES(0, 0, 4294967295u);
    assert(!strcmp(pt.dash.power_status, "1"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    PARSE_ECODES(0, 0, 4294967295u & ~ERR_MM_POWER);
    assert(!strcmp(pt.dash.power_status, "0"));
    assert(!strcmp(pt.dash.asic_status, "1"));

    /* a core that never answers fails the refresh instead of pinning the worker */
    {
        int             peer = silent_peer();
        mw_telemetry    st;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        assert(mw_telemetry_refresh(&st) == -1);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        assert(t1.tv_sec - t0.tv_sec <= 3);
        close(peer);
    }

    {
        static const char head[] =
                "{\"summary\":{\"SUMMARY\":[{\"Elapsed\":565}]},\"pools\":{\"POOLS\":[]},"
                "\"stats\":{\"STATS\":[{\"MMID0Summary\":{\"Sysinfo\":{\"Ver\":\"X3-1\","
                "\"LVer\":\"26071690_434669a\"}},\"HBinfo\":{\"HB_0_Info\":{\"MW0\":[";
        static const char tail[] = "]}}}]},\"version\":{\"VERSION\":[{\"MAC\":\"020000000001\"}]}}";
        size_t            n      = MW_RPC_REPLY_CAP - 1;
        size_t            fill   = n - (sizeof head - 1) - (sizeof tail - 1);
        char             *big    = malloc(n + 1);
        assert(big);
        memcpy(big, head, sizeof head - 1);
        for (size_t i = 0; i < fill; i++)
            big[sizeof head - 1 + i] = (fill - i) % 2 ? '7' : ',';
        if (fill % 2 == 0)
            big[sizeof head - 1] = ' ';
        memcpy(big + n - (sizeof tail - 1), tail, sizeof tail);
        assert(strlen(big) == n && n > 16384);

        int          peer = silent_peer();
        pid_t        pid  = serve_once(peer, big, n);
        mw_telemetry bt2;
        assert(mw_telemetry_refresh(&bt2) == 0);
        assert(!strcmp(bt2.dash.elapsed, "565"));
        assert(!strcmp(bt2.dash.version, "26071690_434669a"));
        assert(!strcmp(bt2.dash.mac, "02:00:00:00:00:01"));
        int st;
        assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        close(peer);
        unlink(MC_API_SOCK_PATH);
        free(big);
    }

    /* zone name from an /etc/localtime link target */
    {
        char z[48];
        assert(mw_tz_from_link("/usr/share/zoneinfo/Asia/Shanghai", z, sizeof z) == 0 &&
                !strcmp(z, "Asia/Shanghai"));
        assert(mw_tz_from_link("../usr/share/zoneinfo/America/Argentina/Buenos_Aires", z,
                       sizeof z) == 0 &&
                !strcmp(z, "America/Argentina/Buenos_Aires"));
        assert(mw_tz_from_link("/usr/share/zoneinfo/UTC", z, sizeof z) == 0 && !strcmp(z, "UTC"));
        assert(mw_tz_from_link("/etc/TZif-copy", z, sizeof z) == -1 && z[0] == 0);
        assert(mw_tz_from_link("/usr/share/zoneinfo/", z, sizeof z) == -1 && z[0] == 0);
        assert(mw_tz_from_link("/usr/share/zoneinfo/Asia/Shanghai", z, 5) == -1 && z[0] == 0);
    }

    printf("telemetry: all assertions passed\n");
    return 0;
}
