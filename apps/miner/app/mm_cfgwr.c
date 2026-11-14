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
#include "mm_factory.h"
#include "cfgwr_io.h"
#include "boardconf.h"

#include "ini.h"
#include "ini_file.h"
#include "pool_config.h"
#include "webauth.h"
#include "osif.h"
#include "hex.h"
#include "fsync_dir.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CFGWR_LISTEN_CNT       5           /* pending connections on the listener, count */
#define CFGWR_STACK_BYTES      (64 * 1024) /* service thread stack, bytes */
#define CFGWR_ACCEPT_BACKOFF_S 1           /* pause after a failed accept, s */
#define CFGWR_PATH_CAP         512         /* local file path buffer, bytes */
#define CFGWR_OWNER_LOCK       MM_CFGWR_SOCK_PATH ".lock"

struct pools_xchg {
    int fd;      /* client connection */
    int enabled; /* the committed pool set holds an enabled pool */
};

static struct {
    pthread_mutex_t lock;                     /* guards hex */
    char            hex[WEBPASS_HEX_LEN + 1]; /* web password SHA-256, hex */
} g_webpass = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct {
    pthread_mutex_t lock; /* guards the members below */
    char            prod[PROD_NAME_CAP];
    char            model[MODEL_NAME_CAP];
    int             activation;
} g_hwcfg = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* service state, set by mm_cfgwr_start before the thread starts, read-only after */
static struct {
    int         listener;   /* listening socket fd, -1 until started */
    int         owner_lock; /* CFGWR_OWNER_LOCK fd holding the flock, -1 until started */
    const char *pools_path;
    const char *systemcfg_path;
} g_cfgwr = { .listener = -1, .owner_lock = -1 };

static atomic_int g_pools_enabled; /* pools.conf holds an enabled pool */

void mm_cfgwr_webpass_set(const char *hex)
{
    pthread_mutex_lock(&g_webpass.lock);
    snprintf(g_webpass.hex, sizeof g_webpass.hex, "%s", hex ? hex : "");
    pthread_mutex_unlock(&g_webpass.lock);
}

void mm_cfgwr_webpass_get(char *out, size_t cap)
{
    pthread_mutex_lock(&g_webpass.lock);
    snprintf(out, cap, "%s", g_webpass.hex);
    pthread_mutex_unlock(&g_webpass.lock);
}

void mm_cfgwr_hwcfg_set(const char *prod, const char *model)
{
    pthread_mutex_lock(&g_hwcfg.lock);
    if (prod && *prod)
        snprintf(g_hwcfg.prod, sizeof g_hwcfg.prod, "%s", prod);
    if (model && *model)
        snprintf(g_hwcfg.model, sizeof g_hwcfg.model, "%s", model);
    pthread_mutex_unlock(&g_hwcfg.lock);
}

void mm_cfgwr_hwcfg_get(char *prod, size_t prodcap, char *model, size_t modelcap)
{
    pthread_mutex_lock(&g_hwcfg.lock);
    if (prod)
        snprintf(prod, prodcap, "%s", g_hwcfg.prod);
    if (model)
        snprintf(model, modelcap, "%s", g_hwcfg.model);
    pthread_mutex_unlock(&g_hwcfg.lock);
}

static void activation_set(int on)
{
    pthread_mutex_lock(&g_hwcfg.lock);
    g_hwcfg.activation = on ? 1 : 0;
    pthread_mutex_unlock(&g_hwcfg.lock);
}

int mm_cfgwr_activation_get(void)
{
    pthread_mutex_lock(&g_hwcfg.lock);
    int on = g_hwcfg.activation;
    pthread_mutex_unlock(&g_hwcfg.lock);
    return on;
}

static const char *systemcfg_or_default(const char *path)
{
    return path && *path ? path : MC_SYSTEMCFG_DEFAULT;
}

static int valid_pools(const pool_set *pools)
{
    if (pools->count < 0 || pools->count > POOL_VALID_NUM)
        return 0;
    for (int index = 0; index < pools->count; index++) {
        const pool_t *pool = &pools->pools[index];
        if (!memchr(pool->url, 0, sizeof pool->url) || !memchr(pool->user, 0, sizeof pool->user) ||
                !memchr(pool->pass, 0, sizeof pool->pass))
            return 0;
        if (pool->slot < (index ? pools->pools[index - 1].slot + 1 : 0) ||
                pool->slot >= POOL_VALID_NUM)
            return 0;
    }
    return 1;
}

static int valid_path(const mm_cfgwr_req *req, const char *path)
{
    return memchr(req->path, 0, sizeof req->path) &&
            (!req->path[0] || strcmp(req->path, path) == 0);
}

static int refused_by_factory(void)
{
    if (!mm_factory_in_progress())
        return 0;
    osif_log(OSIF_LOG_ERR, "config write refused: factory recovery in progress");
    return 1;
}

static int store_commit(const char *section, const char *const *keys, const char *const *values,
        size_t n)
{
    if (mm_cfgstore_commit(section, keys, values, n) == 0)
        return 0;
    if (mm_cfgstore_frozen())
        osif_log(OSIF_LOG_ERR, "config write refused: settings store frozen");
    return -1;
}

static int valid_hwstr(const char *s, size_t cap)
{
    if (!memchr(s, 0, cap) || !*s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    return 1;
}

static int set_activation(ini_t *ini, void *ctx)
{
    (void)ctx;
    return ini_set(ini, "dev_attr", "activation", "1");
}

static int activation_path(char *out, size_t cap, const char *systemcfg_path)
{
    const char *base   = systemcfg_or_default(systemcfg_path);
    const char *slash  = strrchr(base, '/');
    int         dirlen = slash ? (int)(slash - base + 1) : 0;

    return snprintf(out, cap, "%.*sactivation.ini", dirlen, base) < (int)cap ? 0 : -1;
}

void mm_cfgwr_seed(const char *systemcfg_path)
{
    char user[MC_WEBUSER_MAX];
    char pass[WEBPASS_HEX_LEN + 1];
    char prod[PROD_NAME_CAP];
    char model[MODEL_NAME_CAP];

    if (mm_cfgstore_get("syscfg", "webuser", user, sizeof user) == 0 &&
            mm_cfgstore_get("syscfg", "webpass", pass, sizeof pass) == 0)
        mm_cfgwr_webpass_set(pass);
    else
        osif_log(OSIF_LOG_ERR, "no web password in the settings — web login password not set");
    if (mm_cfgstore_get("hwcfg", "prod", prod, sizeof prod) != 0)
        prod[0] = 0;
    if (mm_cfgstore_get("hwcfg", "model", model, sizeof model) != 0)
        model[0] = 0;
    mm_cfgwr_hwcfg_set(prod, model);

#ifndef CAP_ACTIVATION
    activation_set(1);
    return;
#endif
    char act[CFGWR_PATH_CAP];
    if (activation_path(act, sizeof act, systemcfg_path) != 0)
        return;
    ini_t *a = ini_load(act);
    if (!a)
        return;
    const char *v = ini_get(a, "dev_attr", "activation");
    activation_set(v && atoi(v) == 1);
    ini_free(a);
}

static int valid_wp(const mm_cfgwr_req *req)
{
    const char *fmt = req->u.wallpaper.format;

    if (req->path[0])
        return 0;
    if (!memchr(fmt, 0, sizeof req->u.wallpaper.format))
        return 0;
    return strcmp(fmt, "png") == 0 || strcmp(fmt, "gif") == 0;
}

int mm_cfgwr_wp_clear_dir(const char *dir, const char *keep)
{
    DIR *dp = opendir(dir);
    if (!dp) {
        if (errno == ENOENT)
            return 0;
        osif_log(OSIF_LOG_ERR, "Wallpaper folder not readable (%s)", strerror(errno));
        return -1;
    }
    struct dirent *e;
    char           path[CFGWR_PATH_CAP];
    int            rc = 0;

    while ((e = readdir(dp))) {
        if (e->d_type != DT_REG)
            continue;
        if (keep && strcmp(e->d_name, keep) == 0)
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (remove(path) != 0 && errno != ENOENT) {
            osif_log(OSIF_LOG_ERR, "Old wallpaper %s not removed (%s)", e->d_name, strerror(errno));
            rc = -1;
        }
    }
    closedir(dp);
    return rc;
}

static int wp_copy(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rbe");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wbe");
    if (!out) {
        fclose(in);
        return -1;
    }

    char   buf[4096];
    size_t n;
    int    ok = 1;

    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in))
        ok = 0;
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0))
        ok = 0;
    if (fclose(out) != 0)
        ok = 0;
    fclose(in);
    if (!ok)
        unlink(dst);
    return ok ? 0 : -1;
}

static int wp_commit(const mm_cfgwr_wp *wp)
{
    char        src[CFGWR_PATH_CAP], tmp[CFGWR_PATH_CAP], dst[CFGWR_PATH_CAP], tmpname[64];
    struct stat st;

    snprintf(src, sizeof src, MM_CFGWR_WP_STAGE_FMT, MM_CFGWR_WP_STAGE_DIR, (unsigned)wp->uid,
            wp->format);
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        osif_log(OSIF_LOG_ERR, "Wallpaper not published: the uploaded image is missing");
        return -1;
    }
    if (mkdir(MM_CFGWR_WP_DIR, 0755) != 0 && errno != EEXIST) {
        osif_log(OSIF_LOG_ERR, "Wallpaper not published: cannot create the wallpaper folder (%s)",
                strerror(errno));
        return -1;
    }
    snprintf(tmpname, sizeof tmpname, ".wallpaper.%s.tmp", wp->format);
    snprintf(tmp, sizeof tmp, "%s/%s", MM_CFGWR_WP_DIR, tmpname);
    snprintf(dst, sizeof dst, MM_CFGWR_WP_PUB_FMT, MM_CFGWR_WP_DIR, (unsigned)wp->uid, wp->format);

    if (wp_copy(src, tmp) != 0) {
        osif_log(OSIF_LOG_ERR, "Wallpaper not published: copying the uploaded image failed");
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        osif_log(OSIF_LOG_ERR, "Wallpaper not published: renaming the new wallpaper failed (%s)",
                strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (refused_by_factory()) {
        unlink(dst);
        return -1;
    }
    (void)mm_cfgwr_wp_clear_dir(MM_CFGWR_WP_DIR, strrchr(dst, '/') + 1);
    fsync_dir(MM_CFGWR_WP_DIR);
    unlink(src);
    osif_log(OSIF_LOG_OP, "wallpaper published");
    return 0;
}

static int apply_webpass(const mm_cfgwr_req *req, const char *systemcfg_path)
{
    const char *path = systemcfg_or_default(systemcfg_path);
    char        hex[WEBPASS_HEX_LEN + 1];

    if (req->u.webpass[WEBPASS_HEX_LEN] != 0 || !hex_valid(req->u.webpass, WEBPASS_HEX_LEN))
        return -1;
    if (!valid_path(req, path))
        return -1;
    snprintf(hex, sizeof hex, "%.*s", WEBPASS_HEX_LEN, req->u.webpass);
    const char *const key[] = { "webpass" };
    const char *const val[] = { hex };
    if (store_commit("syscfg", key, val, 1) != 0) {
        osif_log(OSIF_LOG_ERR, "Web password not saved: writing the settings failed");
        return -1;
    }
    mm_cfgwr_webpass_set(hex);
    osif_log(OSIF_LOG_OP, "web password saved");
    return 0;
}

static int apply_hwcfg(const mm_cfgwr_req *req, const char *systemcfg_path)
{
    const char    *path = systemcfg_or_default(systemcfg_path);
    mm_cfgwr_hwcfg hw   = req->u.hwcfg;

    if (!valid_hwstr(hw.prod, sizeof hw.prod) || !valid_hwstr(hw.model, sizeof hw.model))
        return -1;
    if (!valid_path(req, path))
        return -1;
    const char *const keys[] = { "prod", "model" };
    const char *const vals[] = { hw.prod, hw.model };
    if (store_commit("hwcfg", keys, vals, 2) != 0) {
        osif_log(OSIF_LOG_ERR, "Hardware settings not saved: writing the settings failed");
        return -1;
    }
    mm_cfgwr_hwcfg_set(hw.prod, hw.model);
    osif_log(OSIF_LOG_OP, "hardware settings saved");
    return 0;
}

static int apply_activation(const mm_cfgwr_req *req, const char *systemcfg_path)
{
    char path[CFGWR_PATH_CAP];

#ifndef CAP_ACTIVATION
    return -1;
#endif
    if (activation_path(path, sizeof path, systemcfg_path) != 0)
        return -1;
    if (!valid_path(req, path))
        return -1;
    if (ini_file_update(path, set_activation, NULL) != 0) {
        osif_log(OSIF_LOG_ERR, "Device activation not saved: writing the activation file failed");
        return -1;
    }
    activation_set(1);
    osif_log(OSIF_LOG_OP, "device activation saved");
    return 0;
}

static int apply_wallpaper_clear(const mm_cfgwr_req *req)
{
    if (req->path[0])
        return -1;
    int rc = mm_cfgwr_wp_clear_dir(MM_CFGWR_WP_DIR, NULL);

    fsync_dir(MM_CFGWR_WP_DIR);
    if (rc != 0)
        return -1;
    osif_log(OSIF_LOG_OP, "wallpaper cleared");
    return 0;
}

int mm_cfgwr_apply(const mm_cfgwr_req *req, const char *systemcfg_path)
{
    if (!req)
        return -1;
    if (req->magic != MM_CFGWR_MAGIC || req->version != MM_CFGWR_VERSION) {
        osif_log(OSIF_LOG_ERR, "Config write refused: request from an incompatible build");
        return -1;
    }
    if (refused_by_factory())
        return -1;

    switch (req->kind) {
    case MM_CFGWR_WEBPASS:
        return apply_webpass(req, systemcfg_path);
    case MM_CFGWR_HWCFG:
        return apply_hwcfg(req, systemcfg_path);
    case MM_CFGWR_ACTIVATION:
        return apply_activation(req, systemcfg_path);
    case MM_CFGWR_WALLPAPER:
        return valid_wp(req) ? wp_commit(&req->u.wallpaper) : -1;
    case MM_CFGWR_WALLPAPER_CLEAR:
        return apply_wallpaper_clear(req);
    default:
        osif_log(OSIF_LOG_ERR, "Config write refused: unknown request type %u", req->kind);
        return -1;
    }
}

static int listener_create(const char *path)
{
    struct sockaddr_un servaddr;
    int                fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        osif_log(OSIF_LOG_ERR, "Config write service unavailable: socket failed (%s)",
                strerror(errno));
        return -1;
    }
    unlink(path);
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sun_family = AF_UNIX;
    strncpy(servaddr.sun_path, path, sizeof servaddr.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&servaddr, sizeof servaddr) < 0) {
        osif_log(OSIF_LOG_ERR, "Config write service unavailable: cannot bind %s (%s)", path,
                strerror(errno));
        close(fd);
        return -1;
    }
    if (chmod(path, S_IRUSR | S_IWUSR) < 0) {
        osif_log(OSIF_LOG_ERR,
                "Config write service unavailable: cannot set "
                "permissions on %s (%s)",
                path, strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    if (listen(fd, CFGWR_LISTEN_CNT) < 0) {
        osif_log(OSIF_LOG_ERR, "Config write service unavailable: listen failed (%s)",
                strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static int pools_enabled(const pool_set *pools)
{
    for (int i = 0; i < pools->count; i++)
        if (pools->pools[i].url[0] && pools->pools[i].enabled)
            return 1;
    return 0;
}

static void pools_enabled_load(const char *path)
{
    pool_set ps = { 0 };

    atomic_store(&g_pools_enabled, pool_config_load(&ps, path) > 0 && pools_enabled(&ps));
}

static int exchange_pools(pool_set *pools, void *ctx)
{
    struct pools_xchg *x = ctx;
    mm_cfgwr_req       request;
    if (refused_by_factory())
        return -1;
    if (cfgwr_write_full(x->fd, pools, sizeof *pools) != 0 ||
            cfgwr_read_full(x->fd, &request, sizeof request) != 0 ||
            request.magic != MM_CFGWR_MAGIC || request.version != MM_CFGWR_VERSION ||
            request.kind != MM_CFGWR_POOLS || !valid_pools(&request.u.pools))
        return -1;
    *pools     = request.u.pools;
    x->enabled = pools_enabled(pools);
    return 0;
}

static const char *cfgwr_kind_name(uint32_t kind)
{
    switch (kind) {
    case MM_CFGWR_UPDATE_POOLS:
        return "pool settings";
    case MM_CFGWR_WEBPASS:
        return "web password";
    case MM_CFGWR_HWCFG:
        return "hardware settings";
    case MM_CFGWR_ACTIVATION:
        return "device activation";
    case MM_CFGWR_WALLPAPER:
        return "wallpaper";
    case MM_CFGWR_WALLPAPER_CLEAR:
        return "wallpaper clear";
    case MM_CFGWR_READ_POOLS:
        return "pool settings read";
    default:
        return "unknown setting";
    }
}

static void set_io_timeout(int cfd, int opt, const char *dir)
{
    struct timeval tv = { MM_CFGWR_IO_TIMEOUT_S, 0 };

    if (setsockopt(cfd, SOL_SOCKET, opt, &tv, sizeof tv) != 0)
        osif_log(OSIF_LOG_WARN,
                "Config write %s timeout (%d s) not set (%s); "
                "a stalled client can block config writes",
                dir, MM_CFGWR_IO_TIMEOUT_S, strerror(errno));
}

static int serve_pools(int cfd, const mm_cfgwr_req *req)
{
    const char *path = g_cfgwr.pools_path;

    if (!valid_path(req, path)) {
        osif_log(OSIF_LOG_ERR, "pools update refused: the request names another pools file");
        return -1;
    }
    struct pools_xchg x  = { .fd = cfd };
    int               rc = pool_config_update(path, exchange_pools, &x);

    if (rc == 0) {
        atomic_store(&g_pools_enabled, x.enabled);
        osif_log(OSIF_LOG_OP, "pool settings saved");
    }
    return rc;
}

static int serve_read_pools(int cfd, const mm_cfgwr_req *req)
{
    const char *path = g_cfgwr.pools_path;
    pool_set    ps;
    int         rc = -1;

    if (!valid_path(req, path)) {
        osif_log(OSIF_LOG_ERR, "pools read refused: the request names another pools file");
        return -1;
    }
    pool_init(&ps, 0, 0);
    if (pool_config_load(&ps, path) >= 0 && cfgwr_write_full(cfd, &ps, sizeof ps) == 0)
        rc = 0;
    explicit_bzero(&ps, sizeof ps);
    return rc;
}

int mm_cfgwr_pool_configured(void)
{
    return atomic_load(&g_pools_enabled);
}

static void serve(int cfd)
{
    mm_cfgwr_req req;
    mm_cfgwr_rep rep = { .magic = MM_CFGWR_MAGIC, .status = -1 };

    set_io_timeout(cfd, SO_RCVTIMEO, "receive");
    set_io_timeout(cfd, SO_SNDTIMEO, "send");

    if (cfgwr_read_full(cfd, &req, sizeof req) == 0) {
        int current = req.magic == MM_CFGWR_MAGIC && req.version == MM_CFGWR_VERSION;

        if (current && req.kind == MM_CFGWR_READ_POOLS)
            rep.status = serve_read_pools(cfd, &req);
        else if (current && req.kind == MM_CFGWR_UPDATE_POOLS)
            rep.status = serve_pools(cfd, &req);
        else
            rep.status = mm_cfgwr_apply(&req, g_cfgwr.systemcfg_path);
        if (rep.status != 0)
            osif_log(OSIF_LOG_ERR, "%s %s", cfgwr_kind_name(req.kind),
                    req.kind == MM_CFGWR_READ_POOLS ? "not sent" : "not saved");
    } else {
        osif_log(OSIF_LOG_ERR, "Config write refused: incomplete request (%s)",
                errno ? strerror(errno) : "peer closed");
    }

    if (cfgwr_write_full(cfd, &rep, sizeof rep) != 0)
        osif_log(OSIF_LOG_WARN, "Config write result not sent back (%s)", strerror(errno));
    close(cfd);
}

static void *cfgwr_thread(void *arg)
{
    (void)arg;

    for (;;) {
        int cfd = accept4(g_cfgwr.listener, NULL, NULL, SOCK_CLOEXEC);

        if (cfd < 0) {
            if (errno != EINTR) {
                osif_log(OSIF_LOG_ERR, "Config write connection failed (%s); retrying",
                        strerror(errno));
                sleep(CFGWR_ACCEPT_BACKOFF_S);
            }
            continue;
        }
        struct ucred peer;
        socklen_t    peer_len = sizeof peer;
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
                peer.uid != geteuid()) {
            close(cfd);
            continue;
        }
        serve(cfd);
    }
    return NULL;
}

int mm_cfgwr_start(const char *pools_path, const char *systemcfg_path)
{
    osif_thread_t *thread = NULL;

    if (g_cfgwr.listener >= 0) {
        osif_log(OSIF_LOG_ERR, "Config write service already running");
        return -1;
    }
    g_cfgwr.pools_path = pools_path && *pools_path ? pools_path : POOL_CONFIG_DEFAULT;
    pools_enabled_load(g_cfgwr.pools_path);
    g_cfgwr.owner_lock = open(CFGWR_OWNER_LOCK, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (g_cfgwr.owner_lock < 0) {
        osif_log(OSIF_LOG_ERR, "Config write service unavailable: cannot open %s (%s)",
                CFGWR_OWNER_LOCK, strerror(errno));
        return -1;
    }
    if (flock(g_cfgwr.owner_lock, LOCK_EX | LOCK_NB) != 0) {
        osif_log(OSIF_LOG_ERR, "Config write service unavailable: cannot lock %s (%s)",
                CFGWR_OWNER_LOCK,
                errno == EWOULDBLOCK ? "held by another writer" : strerror(errno));
        goto err_lock;
    }
    g_cfgwr.listener = listener_create(MM_CFGWR_SOCK_PATH);
    if (g_cfgwr.listener < 0)
        goto err_lock;

    g_cfgwr.systemcfg_path = systemcfg_or_default(systemcfg_path);
    if (osif_thread_create(&thread, "mm_cfgwr", cfgwr_thread, NULL, CFGWR_STACK_BYTES) == OSIF_OK)
        return 0;

    osif_log(OSIF_LOG_ERR,
            "Config write service unavailable this boot: "
            "thread start failed");
    close(g_cfgwr.listener);
    g_cfgwr.listener = -1;
    unlink(MM_CFGWR_SOCK_PATH);
err_lock:
    close(g_cfgwr.owner_lock);
    g_cfgwr.owner_lock = -1;
    return -1;
}
