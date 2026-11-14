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

#include "ini_file.h"
#include "file_atomic.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rbe");
    if (!f)
        return NULL;

    char *text = NULL;
    long  n    = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (text = malloc((size_t)n + 1)) &&
            fread(text, 1, (size_t)n, f) == (size_t)n) {
        *len = (size_t)n;
    } else {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

ini_t *ini_file_load(const char *path)
{
    ini_t *ini = ini_load(path);
    if (!ini && access(path, F_OK) != 0)
        ini = ini_parse("");
    return ini;
}

char *ini_file_serialize(const ini_t *ini)
{
    size_t n    = ini_write(ini, NULL, 0);
    char  *text = malloc(n + 1);
    if (text)
        ini_write(ini, text, n + 1);
    return text;
}

int ini_file_write(const char *path, const char *text, size_t len)
{
    if (!path || !*path || !text)
        return -1;

    int lfd = file_lock(path);
    if (lfd < 0)
        return -1;
    int rc = file_write_atomic(path, text, len);
    close(lfd);
    return rc;
}

int ini_file_update(const char *path, ini_file_update_fn update, void *ctx)
{
    if (!path || !*path || !update)
        return -1;

    int lfd = file_lock(path);
    if (lfd < 0)
        return -1;

    int    rc  = -1;
    ini_t *ini = ini_file_load(path);
    if (ini && update(ini, ctx) == 0) {
        char *text = ini_file_serialize(ini);
        if (text) {
            rc = file_write_atomic(path, text, strlen(text));
            free(text);
        }
    }
    ini_free(ini);
    close(lfd);
    return rc;
}

int ini_file_seed(const char *path, const char *src)
{
    if (!path || !*path || !src)
        return -1;
    if (access(path, F_OK) == 0)
        return 0;

    int lfd = file_lock(path);
    if (lfd < 0)
        return -1;

    int rc = 0;
    if (access(path, F_OK) != 0) {
        size_t len;
        char  *text = read_all(src, &len);
        rc          = text && file_write_atomic(path, text, len) == 0 ? 1 : -1;
        free(text);
    }
    close(lfd);
    return rc;
}

int ini_file_fill(const char *path, const char *src, int *recreated)
{
    if (recreated)
        *recreated = 0;
    if (!path || !*path || !src)
        return -1;

    ini_t *def = ini_load(src);
    if (!def)
        return -1;
    int lfd = file_lock(path);
    if (lfd < 0) {
        ini_free(def);
        return -1;
    }

    errno       = 0;
    ini_t *ini  = ini_load(path);
    int    lost = !ini && (errno == EINVAL || errno == EFBIG);
    if (!ini && (lost || errno == ENOENT))
        ini = ini_parse("");
    int rc = ini ? ini_fill(ini, def) : -1;
    if (rc > 0 || (rc == 0 && lost)) {
        char       *text = ini_file_serialize(ini);
        struct stat st;
        if (lost && lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
            rmdir(path);
        if (!text || file_write_atomic(path, text, strlen(text)) != 0)
            rc = -1;
        free(text);
    }
    if (recreated)
        *recreated = lost && rc >= 0;
    ini_free(ini);
    ini_free(def);
    close(lfd);
    return rc;
}

static const char *head_key(const char *line, size_t len, int *depth, size_t *klen)
{
    while (len && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r'))
        len--;
    if (len && *line == '#')
        return NULL;
    if (len == 1 && *line == '}') {
        if (*depth > 0)
            (*depth)--;
        return NULL;
    }
    if (len && line[len - 1] == '{') {
        (*depth)++;
        return NULL;
    }
    const char *eq = len ? memchr(line, '=', len) : NULL;
    if (*depth || !eq)
        return NULL;
    *klen = (size_t)(eq - line);
    while (*klen && (line[*klen - 1] == ' ' || line[*klen - 1] == '\t'))
        (*klen)--;
    return *klen ? line : NULL;
}

static int has_head_key(const char *text, const char *key, size_t klen)
{
    int depth = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);
        size_t      n;
        const char *k = head_key(p, len, &depth, &n);
        if (k && n == klen && !memcmp(k, key, klen))
            return 1;
        p += len + (eol != NULL);
    }
    return 0;
}

int ini_file_head_fill(const char *path, const char *src)
{
    if (!path || !*path || !src)
        return -1;
    if (access(path, F_OK) != 0)
        return 0;

    size_t slen;
    char  *def = read_all(src, &slen);
    if (!def)
        return -1;
    int lfd = file_lock(path);
    if (lfd < 0) {
        free(def);
        return -1;
    }

    int    rc = -1;
    size_t len;
    char  *text = read_all(path, &len);
    char  *out  = text ? malloc(slen + len + 2) : NULL;
    if (out) {
        size_t pos   = 0;
        int    depth = 0;
        text[len]    = '\0';
        rc           = 0;
        for (const char *p = def; p < def + slen;) {
            const char *eol = memchr(p, '\n', (size_t)(def + slen - p));
            size_t      n   = eol ? (size_t)(eol - p) : (size_t)(def + slen - p);
            size_t      klen;
            const char *k = head_key(p, n, &depth, &klen);
            if (k && !has_head_key(text, k, klen)) {
                memcpy(out + pos, p, n);
                pos += n;
                out[pos++] = '\n';
                rc++;
            }
            p += n + (eol != NULL);
        }
        memcpy(out + pos, text, len);
        if (rc > 0 && file_write_atomic(path, out, pos + len) != 0)
            rc = -1;
    }
    free(out);
    free(text);
    free(def);
    close(lfd);
    return rc;
}
