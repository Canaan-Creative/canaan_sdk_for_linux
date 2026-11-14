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

#define _GNU_SOURCE
#include "mm_cfgwr.h"
#include "mm_cfgstore.h"
#include "cfgwr_client.h"
#include "cfgwr_io.h"
#include "ini.h"
#include "cfgwr_proto.h"
#include "boardconf.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>

#undef MM_CFGWR_SOCK_PATH
#define MM_CFGWR_SOCK_PATH "cfgwr.sock" /* service socket, relative to the cwd */
#undef MM_CFGWR_WP_STAGE_DIR
#define MM_CFGWR_WP_STAGE_DIR "stage" /* wallpaper staging directory, relative to the cwd */
#undef MM_CFGWR_WP_DIR
#define MM_CFGWR_WP_DIR "wp" /* wallpaper directory, relative to the cwd */

static int rename_then_factory(const char *from, const char *to);
#define rename(from, to) rename_then_factory(from, to)
#include "mm_cfgwr.c"
#undef rename
#include "cfgwr_client.c"

static int g_factory_busy;
static int g_factory_on_rename;

static int rename_then_factory(const char *from, const char *to)
{
    int rc = rename(from, to);

    if (g_factory_on_rename)
        g_factory_busy = 1;
    return rc;
}

int mm_factory_in_progress(void)
{
    return g_factory_busy;
}

static int increment(pool_set *pools, void *ctx)
{
    (void)ctx;
    int lock = open("pools.conf.lock", O_RDWR);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    close(lock);
    int value = atoi(pools->pools[0].user);
    usleep(10000);
    snprintf(pools->pools[0].user, sizeof pools->pools[0].user, "%d", value + 1);
    return 0;
}

static int abort_update(pool_set *pools, void *ctx)
{
    (void)ctx;
    pools->count = 0;
    return -1;
}

static int disable_second(pool_set *pools, void *ctx)
{
    (void)ctx;
    assert(pools->count == 2);
    pools->pools[1].enabled = 0;
    return 0;
}

static void *update_thread(void *ctx)
{
    (void)ctx;
    for (int iteration = 0; iteration < 10; iteration++)
        assert(cfgwr_update_pools("pools.conf", increment, NULL) == 0);
    return NULL;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static int count_regular(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;
    struct dirent *e;
    int            n = 0;
    while ((e = readdir(dp)))
        if (e->d_type == DT_REG)
            n++;
    closedir(dp);
    return n;
}

static int slurp(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n]   = 0;
    fclose(f);
    return (int)n;
}

static int connect_service(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, MM_CFGWR_SOCK_PATH);
    assert(connect(fd, (struct sockaddr *)&address, sizeof address) == 0);
    return fd;
}

static int raw_commit(const pool_set *commit)
{
    mm_cfgwr_req request = {
        .magic = MM_CFGWR_MAGIC, .version = MM_CFGWR_VERSION, .kind = MM_CFGWR_UPDATE_POOLS
    };
    pool_set     snapshot;
    mm_cfgwr_rep reply;
    int          fd = connect_service();

    strcpy(request.path, "pools.conf");
    assert(send(fd, &request, sizeof request, MSG_NOSIGNAL) == sizeof request);
    assert(recv(fd, &snapshot, sizeof snapshot, MSG_WAITALL) == sizeof snapshot);
    request.kind    = MM_CFGWR_POOLS;
    request.u.pools = *commit;
    assert(send(fd, &request, sizeof request, MSG_NOSIGNAL) == sizeof request);
    assert(recv(fd, &reply, sizeof reply, MSG_WAITALL) == sizeof reply);
    close(fd);
    assert(reply.magic == MM_CFGWR_MAGIC);
    return reply.status;
}

static void read_full_eof_clears_errno(void)
{
    int  sv[2];
    char buf[8];

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(write(sv[1], "abc", 3) == 3);
    close(sv[1]);
    errno = EINVAL;
    assert(cfgwr_read_full(sv[0], buf, sizeof buf) == -1);
    assert(errno == 0);
    close(sv[0]);
}

static void wp_clear_dir_removes_regular_files(void)
{
    assert(mm_cfgwr_wp_clear_dir("clear", NULL) == 0);
    assert(mkdir("clear", 0700) == 0);
    assert(mkdir("clear/sub", 0700) == 0);
    write_file("clear/sub/inner.png", "I");
    write_file("clear/a.png", "A");
    write_file("clear/b.gif", "B");
    write_file("clear/.hidden", "H");
    assert(mm_cfgwr_wp_clear_dir("clear", "b.gif") == 0);
    assert(access("clear/a.png", F_OK) != 0);
    assert(access("clear/.hidden", F_OK) != 0);
    assert(access("clear/b.gif", F_OK) == 0);
    assert(access("clear/sub/inner.png", F_OK) == 0);
    assert(mm_cfgwr_wp_clear_dir("clear", NULL) == 0);
    assert(access("clear/b.gif", F_OK) != 0);
    assert(access("clear/sub/inner.png", F_OK) == 0);
    assert(unlink("clear/sub/inner.png") == 0);
    assert(rmdir("clear/sub") == 0);
    assert(rmdir("clear") == 0);
}

static void write_pools(const char *json)
{
    FILE *f = fopen("pc.conf", "w");
    assert(f);
    fputs(json, f);
    fclose(f);
}

static int serve_pool_commit(const pool_set *commit)
{
    mm_cfgwr_req request = {
        .magic = MM_CFGWR_MAGIC, .version = MM_CFGWR_VERSION, .kind = MM_CFGWR_POOLS
    };
    pool_set snapshot;
    int      sv[2];

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    request.u.pools = *commit;
    assert(cfgwr_write_full(sv[1], &request, sizeof request) == 0);
    memset(&request, 0, sizeof request);
    int rc = serve_pools(sv[0], &request);
    if (rc == 0)
        assert(cfgwr_read_full(sv[1], &snapshot, sizeof snapshot) == 0);
    close(sv[0]);
    close(sv[1]);
    return rc;
}

static void pool_configured_needs_an_enabled_pool(void)
{
    pool_set commit;

    g_cfgwr.pools_path = "pc.conf";
    pools_enabled_load("pc.conf");
    assert(mm_cfgwr_pool_configured() == 0);
    write_pools("{\"pools\":{\"pool0\":{\"url\":\"\",\"user\":\"w\",\"pass\":\"x\"}}}");
    pools_enabled_load("pc.conf");
    assert(mm_cfgwr_pool_configured() == 0);
    write_pools("{\"pools\":{\"pool1\":{\"url\":\"stratum+tcp://p:3333\",\"user\":\"w\","
                "\"pass\":\"x\",\"disabled\":true}}}");
    pools_enabled_load("pc.conf");
    assert(mm_cfgwr_pool_configured() == 0);
    write_pools("{\"pools\":{\"pool2\":{\"url\":\"stratum+tcp://p:3333\",\"user\":\"w\","
                "\"pass\":\"x\"}}}");
    assert(mm_cfgwr_pool_configured() == 0);
    pools_enabled_load("pc.conf");
    assert(mm_cfgwr_pool_configured() == 1);

    pool_init(&commit, 0, 0);
    assert(pool_add(&commit, "stratum+tcp://p:3333", "w", "x") == 0);
    commit.pools[0].enabled = 0;
    assert(serve_pool_commit(&commit) == 0);
    assert(mm_cfgwr_pool_configured() == 0);
    write_pools("{\"pools\":{\"pool2\":{\"url\":\"stratum+tcp://p:3333\",\"user\":\"w\","
                "\"pass\":\"x\"}}}");
    assert(mm_cfgwr_pool_configured() == 0);
    commit.pools[0].enabled = 1;
    assert(serve_pool_commit(&commit) == 0);
    assert(mm_cfgwr_pool_configured() == 1);
    commit.count = -1;
    assert(serve_pool_commit(&commit) == -1);
    assert(mm_cfgwr_pool_configured() == 1);
    assert(unlink("pc.conf") == 0);
    unlink("pc.conf.lock");
    g_cfgwr.pools_path = NULL;
}

int main(void)
{
    read_full_eof_clears_errno();
    char directory[] = "/tmp/mm-cfgwr-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    wp_clear_dir_removes_regular_files();
    pool_configured_needs_an_enabled_pool();
    assert(mm_cfgstore_open("systemcfg.ini") == 0);
    mm_cfgwr_req request = {
        .magic = MM_CFGWR_MAGIC, .version = MM_CFGWR_VERSION, .kind = MM_CFGWR_WEBPASS
    };
    strcpy(request.u.webpass, "invalid");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    memset(request.u.webpass, 'a', 64);
    request.u.webpass[64] = 0;
    assert(MM_CFGWR_VERSION == 4u);
    request.version = 3u;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    assert(access("systemcfg.ini", F_OK) != 0);
    request.version = MM_CFGWR_VERSION;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    request.kind = MM_CFGWR_POOLS;
    memset(&request.u, 0, sizeof request.u);
    assert(pool_add(&request.u.pools, "stratum+tcp://example:3333", "0", "secret") == 0);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    assert(access("pools.conf", F_OK) != 0);
    request.kind = MM_CFGWR_HWCFG;
    memset(&request.u, 0, sizeof request.u);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    strcpy(request.u.hwcfg.prod, "Avalon Nano3s");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    strcpy(request.u.hwcfg.model, "Nano\n3s");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    memset(request.u.hwcfg.prod, 'p', sizeof request.u.hwcfg.prod);
    strcpy(request.u.hwcfg.model, "Nano3s");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    strcpy(request.u.hwcfg.prod, "Avalon Nano3s");
    assert(mm_cfgstore_set("led", "rgb", "255") == 0);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    assert(mm_cfgstore_set("led", "bright", "42") == 0);
    assert(mm_cfgstore_flush() == 1);
    ini_t *written = ini_load("systemcfg.ini");
    assert(written);
    assert(strspn(ini_get(written, "syscfg", "webpass"), "a") == 64);
    assert(strcmp(ini_get(written, "hwcfg", "prod"), "Avalon Nano3s") == 0);
    assert(strcmp(ini_get(written, "led", "rgb"), "255") == 0);
    assert(strcmp(ini_get(written, "led", "bright"), "42") == 0);
    ini_free(written);
    assert(mm_cfgstore_set("syscfg", "webuser", "admin") == 0);
    mm_cfgwr_seed("systemcfg.ini");
#ifdef CAP_ACTIVATION
    assert(mm_cfgwr_activation_get() == 0);
#else
    assert(mm_cfgwr_activation_get() == 1);
#endif
    assert(mm_cfgstore_flush() == 1);
    char cached[80];
    mm_cfgwr_webpass_get(cached, sizeof cached);
    assert(strspn(cached, "a") == 64);
    mm_cfgwr_hwcfg_get(cached, sizeof cached, NULL, 0);
    assert(strcmp(cached, "Avalon Nano3s") == 0);
    if (geteuid() != 0) {
        mm_cfgwr_req failing = {
            .magic = MM_CFGWR_MAGIC, .version = MM_CFGWR_VERSION, .kind = MM_CFGWR_WEBPASS
        };
        memset(failing.u.webpass, 'c', 64);
        assert(chmod(directory, 0500) == 0);
        assert(mm_cfgwr_apply(&failing, "systemcfg.ini") == -1);
        assert(chmod(directory, 0700) == 0);
        assert(mm_cfgstore_get("syscfg", "webpass", cached, sizeof cached) == 0);
        assert(strspn(cached, "a") == 64);
        mm_cfgwr_webpass_get(cached, sizeof cached);
        assert(strspn(cached, "a") == 64);
        assert(mm_cfgstore_flush() == 0);
    }
    request.kind = MM_CFGWR_POOLS;
    memset(&request.u, 0, sizeof request.u);
    request.kind = MM_CFGWR_ACTIVATION;
    memset(&request.u, 0, sizeof request.u);
#ifdef CAP_ACTIVATION
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    ini_t *activation = ini_load("activation.ini");
    assert(activation);
    assert(strcmp(ini_get(activation, "dev_attr", "activation"), "1") == 0);
    ini_free(activation);
    assert(mm_cfgwr_activation_get() == 1);
    strcpy(request.path, "activation.ini");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    strcpy(request.path, "/data/userconfig/activation.ini");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    request.path[0] = 0;
#else
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    strcpy(request.path, "activation.ini");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    request.path[0] = 0;
    assert(access("activation.ini", F_OK) != 0);
    assert(mm_cfgwr_activation_get() == 1);
#endif

    request.kind = MM_CFGWR_WALLPAPER;
    assert(mkdir("stage", 0700) == 0);
    write_file("stage/wallpaper_7.png.tmp", "IMAGE");
    request.u.wallpaper.uid = 7;
    strcpy(request.u.wallpaper.format, "png");
    strcpy(request.path, "wp/wallpaper.png");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    request.path[0] = 0;
    strcpy(request.u.wallpaper.format, "bmp");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    memcpy(request.u.wallpaper.format, "../png", 7);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    memset(request.u.wallpaper.format, 'p', sizeof request.u.wallpaper.format);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    strcpy(request.u.wallpaper.format, "png");
    request.u.wallpaper.uid = 8;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    request.u.wallpaper.uid = 7;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    char image[64];
    assert(count_regular("wp") == 1);
    assert(slurp("wp/wallpaper_7.png", image, sizeof image) == 5);
    assert(strcmp(image, "IMAGE") == 0);
    assert(access("stage/wallpaper_7.png.tmp", F_OK) != 0);
    write_file("stage/wallpaper_9.gif.tmp", "GIF89a");
    request.u.wallpaper.uid = 9;
    strcpy(request.u.wallpaper.format, "gif");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    assert(count_regular("wp") == 1);
    assert(access("wp/wallpaper_7.png", F_OK) != 0);
    assert(access("wp/wallpaper_9.gif", F_OK) == 0);
    write_file("stage/wallpaper_12.png.tmp", "NEW");
    assert(mkdir("wp/wallpaper_12.png", 0700) == 0);
    request.u.wallpaper.uid = 12;
    strcpy(request.u.wallpaper.format, "png");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    assert(slurp("wp/wallpaper_9.gif", image, sizeof image) == 6);
    assert(strcmp(image, "GIF89a") == 0);
    assert(count_regular("wp") == 1);
    assert(rmdir("wp/wallpaper_12.png") == 0);
    assert(unlink("stage/wallpaper_12.png.tmp") == 0);
    write_file("stage/wallpaper_13.png.tmp", "RACE");
    request.u.wallpaper.uid = 13;
    g_factory_on_rename     = 1;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    g_factory_on_rename = 0;
    g_factory_busy      = 0;
    assert(access("wp/wallpaper_13.png", F_OK) != 0);
    assert(access("wp/.wallpaper.png.tmp", F_OK) != 0);
    assert(unlink("stage/wallpaper_13.png.tmp") == 0);

    request.kind = MM_CFGWR_WALLPAPER_CLEAR;
    memset(&request.u, 0, sizeof request.u);
    strcpy(request.path, "wp");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    request.path[0] = 0;
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    assert(count_regular("wp") == 0);
    if (geteuid() != 0) {
        write_file("wp/stuck.png", "X");
        assert(chmod("wp", 0500) == 0);
        assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
        assert(chmod("wp", 0700) == 0);
        assert(unlink("wp/stuck.png") == 0);
    }
    assert(mkdir("alt", 0700) == 0);
    assert(chdir("alt") == 0);
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == 0);
    write_file("wp", "not a directory");
    assert(mm_cfgwr_apply(&request, "systemcfg.ini") == -1);
    assert(unlink("wp") == 0);
    assert(chdir("..") == 0);
    assert(rmdir("alt") == 0);
    unlink("activation.ini");
    unlink("activation.ini.lock");

    assert(mkdir(MM_CFGWR_SOCK_PATH, 0700) == 0);
    assert(mm_cfgwr_start("pools.conf", "systemcfg.ini") == -1);
    assert(rmdir(MM_CFGWR_SOCK_PATH) == 0);
    FILE *seed = fopen("pools.conf", "w");
    assert(seed);
    assert(fputs("{\"standard\":\"keep-me\",\"pools\":{}}", seed) >= 0);
    assert(fclose(seed) == 0);
    seed = fopen("systemcfg.ini", "w");
    assert(seed);
    assert(fputs("[syscfg]\ntimezone=UTC\n[other]\nkey=keep-me\n", seed) >= 0);
    assert(fclose(seed) == 0);
    assert(mm_cfgstore_reload() == 0);

    int ready[2];
    assert(pipe(ready) == 0);
    pid_t owner = fork();
    assert(owner >= 0);
    if (owner == 0) {
        close(ready[0]);
        assert(mm_cfgwr_start("pools.conf", "systemcfg.ini") == 0);
        assert(mm_cfgwr_start("pools.conf", "systemcfg.ini") == -1);
        assert(write(ready[1], "1", 1) == 1);
        close(ready[1]);
        for (;;)
            pause();
    }
    close(ready[1]);
    char started;
    assert(read(ready[0], &started, 1) == 1);
    close(ready[0]);
    assert(mm_cfgwr_start("pools.conf", "systemcfg.ini") == -1);
    struct stat socket_stat;
    assert(stat(MM_CFGWR_SOCK_PATH, &socket_stat) == 0);
    assert((socket_stat.st_mode & 0777) == 0600);

    pool_set pools;
    pool_init(&pools, 0, 0);
    assert(pool_add(&pools, "stratum+tcp://example:3333", "0", "secret") == 0);
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &pools) == 0);
    assert(cfgwr_update_pools("wrong-path", increment, NULL) == -1);
    assert(cfgwr_update_pools("wrong-path", pool_config_apply_slots, &pools) == -1);
    char     before[2048], after[2048];
    pool_set bad;
    assert(slurp("pools.conf", before, sizeof before) > 0);
    memset(&bad, 0, sizeof bad);
    bad.count = -1;
    assert(raw_commit(&bad) == -1);
    bad.count = POOL_VALID_NUM + 1;
    assert(raw_commit(&bad) == -1);
    bad.count = 1;
    memset(bad.pools[0].url, 'x', POOL_URL_MAX);
    assert(raw_commit(&bad) == -1);
    memset(&bad, 0, sizeof bad);
    bad.count         = 2;
    bad.pools[0].slot = 1;
    bad.pools[1].slot = 1;
    assert(raw_commit(&bad) == -1);
    bad.pools[1].slot = POOL_VALID_NUM;
    assert(raw_commit(&bad) == -1);
    {
        mm_cfgwr_req old = {
            .magic = MM_CFGWR_MAGIC, .version = 3u, .kind = MM_CFGWR_UPDATE_POOLS
        };
        mm_cfgwr_rep reply;
        int          fd = connect_service();
        strcpy(old.path, "pools.conf");
        assert(send(fd, &old, sizeof old, MSG_NOSIGNAL) == sizeof old);
        assert(recv(fd, &reply, sizeof reply, MSG_WAITALL) == sizeof reply);
        close(fd);
        assert(reply.magic == MM_CFGWR_MAGIC && reply.status == -1);
    }
    assert(slurp("pools.conf", after, sizeof after) > 0);
    assert(strcmp(before, after) == 0);
    pthread_t first, second;
    assert(pthread_create(&first, NULL, update_thread, NULL) == 0);
    assert(pthread_create(&second, NULL, update_thread, NULL) == 0);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(pool_config_load(&pools, "pools.conf") == 1);
    assert(pools.count == 1);
    assert(strcmp(pools.pools[0].user, "20") == 0);
    assert(cfgwr_update_pools("pools.conf", abort_update, NULL) == -1);
    assert(cfgwr_write_webpass("bad", "systemcfg.ini") == -1);
    char digest[65];
    memset(digest, 'b', 64);
    digest[64] = 0;
    assert(cfgwr_write_webpass(digest, "systemcfg.ini") == 0);
    assert(cfgwr_write_webpass(digest, "wrong-path") == -1);
    assert(cfgwr_write_hwcfg("", "Nano3s", "systemcfg.ini") == -1);
    assert(cfgwr_write_hwcfg("Avalon Nano3s", "", "systemcfg.ini") == -1);
    assert(cfgwr_write_hwcfg("Avalon Nano3s", "Nano3s", "wrong-path") == -1);
    assert(cfgwr_write_hwcfg("Avalon Nano3s", "Nano3s", "systemcfg.ini") == 0);
    ini_t *config = ini_load("systemcfg.ini");
    assert(config);
    assert(strcmp(ini_get(config, "syscfg", "webpass"), digest) == 0);
    assert(strcmp(ini_get(config, "syscfg", "timezone"), "UTC") == 0);
    assert(strcmp(ini_get(config, "other", "key"), "keep-me") == 0);
    assert(strcmp(ini_get(config, "hwcfg", "prod"), "Avalon Nano3s") == 0);
    assert(strcmp(ini_get(config, "hwcfg", "model"), "Nano3s") == 0);
    ini_free(config);
    assert(pool_config_load(&pools, "pools.conf") == 1 && pools.count == 1);
    assert(strcmp(pools.pools[0].user, "20") == 0);

    pool_set web;
    pool_init(&web, 0, 0);
    assert(pool_add(&web, "stratum+tcp://example:3333", "20", "secret") == 0);
    assert(pool_add(&web, "stratum+tcp://backup:3333", "b", "b") == 1);
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &web) == 0);
    assert(cfgwr_update_pools("pools.conf", disable_second, NULL) == 0);
    snprintf(web.pools[0].url, sizeof web.pools[0].url, "%s", "stratum+tcp://edited:3333");
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &web) == 0);
    assert(pool_config_load(&pools, "pools.conf") == 2);
    assert(strcmp(pools.pools[0].url, "stratum+tcp://edited:3333") == 0);
    assert(strcmp(pools.pools[0].user, "20") == 0 && pools.pools[0].enabled);
    assert(strcmp(pools.pools[1].url, "stratum+tcp://backup:3333") == 0);
    assert(pools.pools[1].slot == 1 && !pools.pools[1].enabled);
    snprintf(web.pools[1].url, sizeof web.pools[1].url, "%s", "stratum+tcp://fresh:3333");
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &web) == 0);
    assert(pool_config_load(&pools, "pools.conf") == 2);
    assert(strcmp(pools.pools[1].url, "stratum+tcp://fresh:3333") == 0);
    assert(pools.pools[1].slot == 1 && pools.pools[1].enabled);
    assert(pools.pools[0].enabled);
    {
        pool_set    got;
        struct stat st_before, st_after;

        assert(slurp("pools.conf", before, sizeof before) > 0);
        assert(stat("pools.conf", &st_before) == 0);
        assert(cfgwr_read_pools("pools.conf", &got) == 0 && got.count == 2);
        assert(!strcmp(got.pools[0].url, "stratum+tcp://edited:3333") &&
                !strcmp(got.pools[0].pass, "secret") && got.pools[0].slot == 0);
        assert(!strcmp(got.pools[1].url, "stratum+tcp://fresh:3333") &&
                !strcmp(got.pools[1].pass, "b") && got.pools[1].slot == 1);
        assert(cfgwr_read_pools("wrong-path", &got) == -1);
        assert(cfgwr_read_pools(NULL, &got) == -1);
        assert(cfgwr_read_pools("pools.conf", NULL) == -1);
        assert(slurp("pools.conf", after, sizeof after) > 0 && !strcmp(before, after));
        assert(stat("pools.conf", &st_after) == 0 && st_after.st_ino == st_before.st_ino &&
                st_after.st_mtim.tv_sec == st_before.st_mtim.tv_sec &&
                st_after.st_mtim.tv_nsec == st_before.st_mtim.tv_nsec);

        mm_cfgwr_req old = { .magic = MM_CFGWR_MAGIC, .version = 3u, .kind = MM_CFGWR_READ_POOLS };
        mm_cfgwr_rep reply;
        int          fd = connect_service();
        strcpy(old.path, "pools.conf");
        assert(send(fd, &old, sizeof old, MSG_NOSIGNAL) == sizeof old);
        assert(recv(fd, &reply, sizeof reply, MSG_WAITALL) == sizeof reply);
        close(fd);
        assert(reply.magic == MM_CFGWR_MAGIC && reply.status == -1);
    }

#ifdef CAP_ACTIVATION
    assert(cfgwr_write_activation("activation.ini") == 0);
    activation = ini_load("activation.ini");
    assert(activation);
    assert(strcmp(ini_get(activation, "dev_attr", "activation"), "1") == 0);
    ini_free(activation);
#else
    assert(cfgwr_write_activation("activation.ini") == -1);
    assert(access("activation.ini", F_OK) != 0);
#endif
    assert(cfgwr_write_activation("wrong-path") == -1);
    write_file("stage/wallpaper_11.png.tmp", "VIA-SOCKET");
    assert(cfgwr_write_wallpaper(11, "png") == 0);
    assert(count_regular("wp") == 1);
    assert(slurp("wp/wallpaper_11.png", image, sizeof image) == 10);
    assert(cfgwr_write_wallpaper(11, "png") == -1);
    assert(cfgwr_write_wallpaper(11, "toolongformat") == -1);
    assert(cfgwr_clear_wallpaper() == 0);
    assert(count_regular("wp") == 0);

    int fd = connect_service();
    assert(send(fd, &request, 1, MSG_NOSIGNAL) == 1);
    close(fd);
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &pools) == 0);
    request.kind    = MM_CFGWR_POOLS;
    request.u.pools = pools;
    fd              = connect_service();
    assert(send(fd, &request, sizeof request, MSG_NOSIGNAL) == sizeof request);
    close(fd);
    assert(cfgwr_write_webpass(digest, "systemcfg.ini") == 0);
    seed = fopen("pools.conf", "r");
    assert(seed);
    char   document[2048];
    size_t length    = fread(document, 1, sizeof document - 1, seed);
    document[length] = 0;
    assert(fclose(seed) == 0);
    assert(strstr(document, "\"standard\":\"keep-me\""));

    fd = connect_service();
    assert(send(fd, &request, 1, MSG_NOSIGNAL) == 1);
    struct pollfd stalled = { .fd = fd, .events = POLLIN };
    assert(poll(&stalled, 1, (MM_CFGWR_IO_TIMEOUT_S + 5) * 1000) == 1);
    mm_cfgwr_rep refused;
    assert(recv(fd, &refused, sizeof refused, MSG_WAITALL) == sizeof refused);
    assert(refused.magic == MM_CFGWR_MAGIC && refused.status == -1);
    close(fd);
    assert(cfgwr_update_pools("pools.conf", pool_config_apply_slots, &pools) == 0);
    assert(kill(owner, SIGTERM) == 0);
    int status;
    assert(waitpid(owner, &status, 0) == owner);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(cfgwr_write_webpass(digest, "systemcfg.ini") == -1);

    unlink("pools.conf");
    unlink("pools.conf.lock");
    unlink("systemcfg.ini");
    unlink("systemcfg.ini.lock");
    unlink("activation.ini");
    unlink("activation.ini.lock");
    unlink(MM_CFGWR_SOCK_PATH);
    unlink(MM_CFGWR_SOCK_PATH ".lock");
    assert(rmdir("stage") == 0);
    assert(rmdir("wp") == 0);
    assert(chdir("/") == 0);
    assert(rmdir(directory) == 0);
    if (geteuid() == 0) {
        printf("SKIP: unwritable config and wallpaper directories (root)\n");
        return TST_SKIP;
    }
    return 0;
}
