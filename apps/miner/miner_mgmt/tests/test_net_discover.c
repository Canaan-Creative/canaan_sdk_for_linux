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

#define _DEFAULT_SOURCE
#include "net_discover.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL: %s\n", msg); \
            fails++; \
        } else \
            printf("  ok: %s\n", msg); \
    } while (0)

#define FD_SCAN 1024 /* Highest fd number + 1 that scan_fds() records in its seen[] map */

static int fails;

int net_state_ip(char *ip, size_t cap)
{
    if (ip && cap)
        ip[0] = '\0';
    return -1;
}

static int scan_fds(unsigned char *seen, int want_new_socket)
{
    DIR           *d = opendir("/proc/self/fd");
    struct dirent *e;
    int            hit = -1;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char    link[64], tgt[64];
        int     fd = atoi(e->d_name);
        ssize_t n;

        if (e->d_name[0] < '0' || e->d_name[0] > '9' || fd >= FD_SCAN || fd == dirfd(d))
            continue;
        if (!want_new_socket) {
            seen[fd] = 1;
            continue;
        }
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        n = readlink(link, tgt, sizeof tgt - 1);
        if (!seen[fd] && n > 0 && !strncmp(tgt, "socket:", 7)) {
            hit = fd;
            break;
        }
    }
    closedir(d);
    return hit;
}

int main(void)
{
    char out[128];

    size_t n = net_discover_reply("get_ip$$$$", "nan3s_1a2b", "192.168.1.5", out, sizeof out);
    CHECK(n == strlen(out), "reply length excludes the NUL");
    CHECK(!strcmp(out, "{\"device\":\"nan3s_1a2b\",\"ip\":\"192.168.1.5\"}"),
            "byte-exact API reply {device,ip}");

    n = net_discover_reply("get_ip$$$$\r\n", "dev", "10.0.0.1", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"device\":\"dev\",\"ip\":\"10.0.0.1\"}"),
            "probe with trailing bytes -> answered (prefix match)");

    CHECK(net_discover_reply("get_ip", "dev", "1.2.3.4", out, sizeof out) == 0,
            "short of the full probe -> no reply");
    CHECK(net_discover_reply("hello", "dev", "1.2.3.4", out, sizeof out) == 0,
            "unrelated datagram -> no reply");
    CHECK(net_discover_reply("", "dev", "1.2.3.4", out, sizeof out) == 0,
            "empty datagram -> no reply");

    n = net_discover_reply("get_ip$$$$", NULL, NULL, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"device\":\"\",\"ip\":\"\"}"),
            "NULL dev/ip -> empty fields, still valid JSON");

    n = net_discover_reply("get_ip$$$$", "nan3s_1a2b", "", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "{\"device\":\"nan3s_1a2b\",\"ip\":\"\"}"),
            "empty ip -> answered with blank ip field");

    CHECK(net_discover_reply(NULL, "d", "i", out, sizeof out) == 0, "NULL req -> 0");
    CHECK(net_discover_reply("get_ip$$$$", "d", "i", NULL, sizeof out) == 0, "NULL out -> 0");
    CHECK(net_discover_reply("get_ip$$$$", "d", "i", out, 0) == 0, "zero cap -> 0");

    char small[10];
    memset(small, 0x7f, sizeof small);
    n = net_discover_reply("get_ip$$$$", "averylongdevicename", "192.168.1.5", small, sizeof small);
    CHECK(n == sizeof small - 1, "truncated reply length == cap-1");
    CHECK(small[n] == '\0', "truncated reply is NUL-terminated at the reported length");

    {
        struct sockaddr_in  a1 = { .sin_family = AF_INET }, m1 = { .sin_family = AF_INET };
        struct sockaddr_in  a2 = { .sin_family = AF_INET }, m2 = { .sin_family = AF_INET };
        struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
        struct ifaddrs      i2 = { .ifa_addr = (struct sockaddr *)&a2,
                 .ifa_netmask                = (struct sockaddr *)&m2 };
        struct ifaddrs      i6 = { .ifa_next = &i2,
                 .ifa_addr                   = (struct sockaddr *)&a6,
                 .ifa_netmask                = (struct sockaddr *)&a6 };
        struct ifaddrs      i0 = { .ifa_next = &i6 };
        struct ifaddrs      i1 = { .ifa_next = &i0,
                 .ifa_addr                   = (struct sockaddr *)&a1,
                 .ifa_netmask                = (struct sockaddr *)&m1 };
        struct in_addr      src;

        inet_pton(AF_INET, "192.168.1.5", &a1.sin_addr);
        inet_pton(AF_INET, "255.255.255.0", &m1.sin_addr);
        inet_pton(AF_INET, "10.20.0.7", &a2.sin_addr);
        inet_pton(AF_INET, "255.255.0.0", &m2.sin_addr);

        inet_pton(AF_INET, "192.168.1.200", &src);
        CHECK(net_discover_local(&i1, src), "sender in the first interface's subnet -> local");
        inet_pton(AF_INET, "10.20.99.1", &src);
        CHECK(net_discover_local(&i1, src), "sender in a later interface's subnet -> local");
        inet_pton(AF_INET, "192.168.2.5", &src);
        CHECK(!net_discover_local(&i1, src), "sender in a neighbouring subnet -> not local");
        inet_pton(AF_INET, "8.8.8.8", &src);
        CHECK(!net_discover_local(&i1, src), "internet sender -> not local");
        CHECK(!net_discover_local(NULL, src), "no interfaces -> not local");
        CHECK(!net_discover_local(&i0, src), "interfaces without an IPv4 address -> not local");
    }

    {
        net_discover_seen_t seen[NET_DISCOVER_SRC_MAX] = { 0 };
        struct in_addr      src, other;
        int                 ok = 0;

        inet_pton(AF_INET, "192.168.1.200", &src);
        inet_pton(AF_INET, "192.168.1.201", &other);
        CHECK(net_discover_rate_ok(seen, src, 5000), "first probe from a sender -> reply");
        for (int i = 0; i < 100; i++)
            ok += net_discover_rate_ok(seen, src, 5000 + i * 9);
        CHECK(ok == 0, "the same sender within one second -> no reply");
        CHECK(net_discover_rate_ok(seen, other, 5100),
                "another sender within that second -> reply");
        CHECK(!net_discover_rate_ok(seen, src, 5999), "the first sender at 999 ms -> no reply");
        CHECK(net_discover_rate_ok(seen, src, 6000), "the first sender at 1000 ms -> reply");
        CHECK(!net_discover_rate_ok(seen, other, 6099), "the second sender at 999 ms -> no reply");
    }

    {
        net_discover_seen_t seen[NET_DISCOVER_SRC_MAX] = { 0 };
        struct in_addr      src;
        int                 ok = 0;

        for (int i = 0; i < 100; i++) {
            src.s_addr = htonl(0x0a000001 + i);
            ok += net_discover_rate_ok(seen, src, 9000 + i);
        }
        CHECK(ok == NET_DISCOVER_SRC_MAX, "at most 10 senders answered within one second");
        src.s_addr = htonl(0x0a000001 + 50);
        CHECK(net_discover_rate_ok(seen, src, 10000),
                "a new sender once the first reply is a second old -> reply");
        src.s_addr = htonl(0x0a000001 + 51);
        CHECK(!net_discover_rate_ok(seen, src, 10000), "...but only into the slot that freed");
    }

    /* the long-lived UDP socket must not leak into children mm_miner starts */
    {
        unsigned char seen[FD_SCAN] = { 0 };
        int           fd            = -1;

        scan_fds(seen, 0);
        net_discover_start("dev");
        for (int i = 0; i < 200 && fd < 0; i++) {
            nanosleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL);
            fd = scan_fds(seen, 1);
        }
        CHECK(fd >= 0, "net_discover_start opens a UDP socket");
        int fl = fd >= 0 ? fcntl(fd, F_GETFD) : -1;
        CHECK(fl >= 0 && (fl & FD_CLOEXEC), "the discovery socket is close-on-exec");
    }

    if (fails) {
        fprintf(stderr, "net_discover: %d FAILED\n", fails);
        return 1;
    }
    printf("net_discover: all passed\n");
    return 0;
}
