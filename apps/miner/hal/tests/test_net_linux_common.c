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

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "net_linux_paths.h"

#undef MAIN_DHCP_PIDF
#undef MM_PROC_DIR
/* udhcpc PID file path overridden to a file in the working directory */
#define MAIN_DHCP_PIDF "test_net_linux_udhcpc.pid"
/* fake procfs directory holding the FAKE_PID entry */
#define TST_PROC_FAKE "test_net_linux_proc/"
#define MM_PROC_DIR   tst_proc   /* procfs directory overridden to tst_proc */
#define TST_CMDS      16         /* capacity of the system() command log */
#define FAKE_PID      0x7ffffff0 /* PID of the simulated udhcpc, never a real process */
#define CHECK(c, m) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "  FAIL: %s\n", m); \
            fails++; \
        } else \
            printf("  ok: %s\n", m); \
    } while (0)

static const char *tst_proc = "/proc/";     /* procfs directory net_linux_common.c reads */
static char        tst_cmds[TST_CMDS][256]; /* commands passed to system() */
static int         tst_ncmds;               /* system() calls */
/* FAKE_PID alive; ignores SIGTERM; probes it survives SIGKILL; SIGKILL received */
static int   fake_alive, fake_ignore_term, fake_kill_lag, fake_killed;
static int   fake_alive_at_cmd; /* 1 when FAKE_PID was alive during any system() call */
static int   tst_sigs[8];       /* first 8 non-zero signals sent */
static pid_t tst_sig_pid[8];    /* target PID of each logged signal */
static int   tst_nsigs;         /* count of non-zero signals sent */
/* IPv4 address the ioctl returns, NULL fails it with EADDRNOTAVAIL */
static const char   *tst_ifaddr;
static int           tst_nioctl;
static unsigned long tst_ioctl_req;          /* request of the last ioctl */
static char          tst_ioctl_if[IFNAMSIZ]; /* interface name of the last ioctl */
static int           fails;

static int tst_kill(pid_t p, int sig);
static int tst_ioctl(int fd, unsigned long req, void *arg);
static int tst_system(const char *cmd);

#define system(c)      tst_system(c) /* routes system() to the command log */
#define kill(p, s)     tst_kill((p), (s))
#define ioctl(f, r, a) tst_ioctl((f), (r), (a))
#include "net_linux_common.c"
#undef ioctl
#undef kill
#undef system

static int tst_ioctl(int fd, unsigned long req, void *arg)
{
    struct ifreq       *ifr = arg;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;

    (void)fd;
    tst_nioctl++;
    tst_ioctl_req = req;
    snprintf(tst_ioctl_if, sizeof tst_ioctl_if, "%s", ifr->ifr_name);
    if (!tst_ifaddr) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, tst_ifaddr, &sin->sin_addr);
    return 0;
}

static int tst_system(const char *cmd)
{
    if (tst_ncmds < TST_CMDS)
        snprintf(tst_cmds[tst_ncmds], sizeof tst_cmds[0], "%s", cmd);
    tst_ncmds++;
    fake_alive_at_cmd |= fake_alive;
    return 0;
}

static int tst_kill(pid_t p, int sig)
{
    if (sig != 0) {
        if (tst_nsigs < 8) {
            tst_sig_pid[tst_nsigs] = p;
            tst_sigs[tst_nsigs]    = sig;
        }
        tst_nsigs++;
    }
    if (p != FAKE_PID)
        return kill(p, sig);
    if (!fake_alive) {
        errno = ESRCH;
        return -1;
    }
    if (sig == SIGKILL)
        fake_killed = 1;
    if (sig == 0 && fake_killed && fake_kill_lag-- <= 0)
        fake_alive = 0;
    if (sig == SIGTERM && !fake_ignore_term)
        fake_alive = 0;
    return fake_alive || sig != 0 ? 0 : (errno = ESRCH, -1);
}

static pid_t spawn_client(const char *comm)
{
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_NAME, comm, 0, 0, 0);
        for (;;)
            pause();
    }
    return p;
}

static int alive(pid_t p)
{
    return kill(p, 0) == 0;
}

static void write_pidfile(pid_t p)
{
    FILE *f = fopen(MAIN_DHCP_PIDF, "w");
    if (!f) {
        fprintf(stderr, "  FAIL: cannot write %s\n", MAIN_DHCP_PIDF);
        exit(2);
    }
    fprintf(f, "%ld\n", (long)p);
    fclose(f);
}

static void fake_comm(const char *comm)
{
    char  path[128];
    FILE *f;

    mkdir(TST_PROC_FAKE, 0755);
    snprintf(path, sizeof path, TST_PROC_FAKE "%d", FAKE_PID);
    mkdir(path, 0755);
    snprintf(path, sizeof path, TST_PROC_FAKE "%d/comm", FAKE_PID);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", comm);
        fclose(f);
    }
}

static void fake_clear(void)
{
    char path[128];

    snprintf(path, sizeof path, TST_PROC_FAKE "%d/comm", FAKE_PID);
    remove(path);
    snprintf(path, sizeof path, TST_PROC_FAKE "%d", FAKE_PID);
    rmdir(path);
    rmdir(TST_PROC_FAKE);
}

static void reset(void)
{
    tst_ncmds  = 0;
    tst_nsigs  = 0;
    tst_nioctl = 0;
    tst_ifaddr = NULL;
    fake_alive = fake_ignore_term = fake_kill_lag = fake_killed = 0;
    fake_alive_at_cmd                                           = 0;
}

static int any_cmd_has(const char *needle)
{
    for (int i = 0; i < tst_ncmds && i < TST_CMDS; i++)
        if (strstr(tst_cmds[i], needle))
            return 1;
    return 0;
}

static int pidfile_exists(void)
{
    struct stat st;
    return stat(MAIN_DHCP_PIDF, &st) == 0;
}

static void reap(pid_t p)
{
    if (p > 0) {
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
    }
}

int main(void)
{
    const struct timespec settle = { 0, 50 * 1000000L };
    pid_t                 own, other;

    signal(SIGCHLD, SIG_IGN);
    own   = spawn_client("udhcpc");
    other = spawn_client("udhcpc");
    if (own <= 0 || other <= 0) {
        fprintf(stderr, "  FAIL: fork\n");
        reap(own);
        reap(other);
        return 2;
    }
    nanosleep(&settle, NULL);

    reset();
    write_pidfile(own);
    net_linux_dhcp();
    CHECK(tst_nsigs == 1 && tst_sig_pid[0] == own && tst_sigs[0] == SIGTERM && !alive(own),
            "the udhcpc named by this interface's pidfile is stopped");
    CHECK(alive(other), "a DHCP client on another interface keeps running");
    CHECK(!any_cmd_has("pidof") && !any_cmd_has("killall"),
            "no command stops DHCP clients by program name");
    CHECK(any_cmd_has("-i " MM_NET_IFACE) && any_cmd_has("-p " MAIN_DHCP_PIDF),
            "the restarted client writes the pidfile the next stop reads");
    CHECK(!pidfile_exists(), "the stopped client's pidfile is removed");

    tst_proc = TST_PROC_FAKE;

    reset();
    write_pidfile(FAKE_PID);
    fake_comm("udhcpc");
    CHECK(net_linux_set_static("192.168.1.2", "255.255.255.0", "192.168.1.1", "") == 0 &&
                    tst_ncmds == 4,
            "a dotted-quad static config runs its four commands");
    CHECK(!strcmp(tst_cmds[0], "ifconfig " MM_NET_IFACE " 192.168.1.2 netmask 255.255.255.0") &&
                    !strcmp(tst_cmds[1], "route del default 2>/dev/null") &&
                    !strcmp(tst_cmds[2], "route add default gw 192.168.1.1") &&
                    !strcmp(tst_cmds[3], "printf 'nameserver 192.168.1.1\\n' > /etc/resolv.conf"),
            "a canonical static config reaches the commands unchanged");
    CHECK(tst_nsigs == 0 && !pidfile_exists() && !any_cmd_has("pidof"),
            "a stale pidfile naming an exited pid signals nothing and is removed");

    {
        static const char *const bad[] = {
            "1.2.3.4;id",
            "$(reboot)",
            "`reboot`",
            "1.2.3.4 -x",
            "1.2.3.4|sh",
            "1.2.3.4'",
            "1.2.3",
            "256.1.1.1",
            "192.168.1.256",
            "1.2.3.4.5",
            "1.2.3.04x",
            "1.2.3.0004",
            "1..3.4",
            "1.2.3.-4",
            " 1.2.3.4",
            "0x1.2.3.4",
            "",
        };
        int ran = 0, accepted = 0;

        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            for (int f = 0; f < 4; f++) {
                const char *v[4] = { "192.168.1.2", "255.255.255.0", "192.168.1.1", "8.8.8.8" };

                if (f == 3 && !bad[i][0])
                    continue;
                v[f] = bad[i];
                reset();
                write_pidfile(FAKE_PID);
                fake_comm("udhcpc");
                fake_alive = 1;
                if (net_linux_set_static(v[0], v[1], v[2], v[3]) == 0)
                    accepted++;
                ran += tst_ncmds + tst_nsigs + !pidfile_exists();
            }
        }
        CHECK(accepted == 0 && ran == 0,
                "a static field that is not a dotted quad of 1-3 digit decimal "
                "octets up to 255 is refused before the DHCP client is stopped "
                "or any command runs");
    }

    reset();
    CHECK(net_linux_set_static("192.168.001.002", "255.255.255.000", "192.168.001.001",
                  "008.008.004.004") == 0 &&
                    tst_ncmds == 4 &&
                    !strcmp(tst_cmds[0],
                            "ifconfig " MM_NET_IFACE " 192.168.1.2 netmask 255.255.255.0") &&
                    !strcmp(tst_cmds[2], "route add default gw 192.168.1.1") &&
                    !strcmp(tst_cmds[3], "printf 'nameserver 8.8.4.4\\n' > /etc/resolv.conf"),
            "leading-zero static fields run in canonical form");

    reset();
    CHECK(net_linux_set_static("010.000.001.002", "255.000.000.000", "010.000.001.001", "") == 0 &&
                    tst_ncmds == 4 &&
                    !strcmp(tst_cmds[0], "ifconfig " MM_NET_IFACE " 10.0.1.2 netmask 255.0.0.0") &&
                    !strcmp(tst_cmds[2], "route add default gw 10.0.1.1") &&
                    !strcmp(tst_cmds[3], "printf 'nameserver 10.0.1.1\\n' > /etc/resolv.conf"),
            "a leading-zero octet is decimal, not octal");

    reset();
    net_linux_write_resolv("8.8.8.8");
    net_linux_write_resolv("8.8.8.8' > /etc/passwd; echo '");
    net_linux_write_resolv(NULL);
    net_linux_write_resolv("");
    net_linux_write_resolv("1.2.3.04x");
    net_linux_write_resolv("1.1.1.256");
    net_linux_write_resolv("008.008.004.004");
    CHECK(tst_ncmds == 2 &&
                    !strcmp(tst_cmds[0], "printf 'nameserver 8.8.8.8\\n' > /etc/resolv.conf") &&
                    !strcmp(tst_cmds[1], "printf 'nameserver 8.8.4.4\\n' > /etc/resolv.conf"),
            "only a dotted-quad DNS server is written to resolv.conf, in "
            "canonical form");

    reset();
    write_pidfile(FAKE_PID);
    fake_comm("sshd");
    fake_alive = 1;
    net_linux_dhcp();
    CHECK(tst_nsigs == 0 && fake_alive && !pidfile_exists(),
            "a pidfile whose pid now belongs to another program signals "
            "nothing and is removed");

    reset();
    write_pidfile(FAKE_PID);
    fake_comm("udhcpc");
    fake_alive       = 1;
    fake_ignore_term = 1;
    fake_kill_lag    = 3;
    net_linux_dhcp();
    CHECK(tst_nsigs == 2 && tst_sig_pid[0] == FAKE_PID && tst_sigs[0] == SIGTERM &&
                    tst_sig_pid[1] == FAKE_PID && tst_sigs[1] == SIGKILL,
            "a udhcpc that outlives SIGTERM for 2 s is sent SIGKILL");
    CHECK(!fake_alive && !fake_alive_at_cmd && !pidfile_exists(),
            "the stop waits for the SIGKILLed udhcpc to exit before the "
            "address is touched");
    fake_clear();
    tst_proc = "/proc/";

    reset();
    net_linux_dhcp();
    CHECK(alive(other) && tst_ncmds == 2 && tst_nsigs == 0,
            "with no pidfile nothing is stopped; only ifconfig and udhcpc run");

    {
        char addr[INET_ADDRSTRLEN];

        reset();
        tst_ifaddr = "192.168.1.77";
        CHECK(net_linux_get_addr(addr, sizeof addr) == 0 && !strcmp(addr, "192.168.1.77") &&
                        tst_nioctl == 1 && tst_ioctl_req == SIOCGIFADDR &&
                        !strcmp(tst_ioctl_if, MM_NET_IFACE) && tst_ncmds == 0,
                "the address is read with one SIOCGIFADDR on the main "
                "interface as a dotted quad, running no command");

        reset();
        strcpy(addr, "x");
        CHECK(net_linux_get_addr(addr, sizeof addr) == -1 && !addr[0] && tst_nioctl == 1,
                "an interface with no IPv4 address reports none");

        reset();
        tst_ifaddr = "192.168.100.177";
        CHECK(net_linux_get_addr(addr, 15) == -1 && !addr[0],
                "an address that does not fit the buffer is not reported");
        CHECK(net_linux_get_addr(addr, 16) == 0 && !strcmp(addr, "192.168.100.177"),
                "an address that fits with its NUL is reported");

        reset();
        CHECK(net_linux_get_addr(addr, 0) == -1 && net_linux_get_addr(NULL, sizeof addr) == -1 &&
                        tst_nioctl == 0,
                "no buffer, no probe");
    }

    reap(own);
    reap(other);
    remove(MAIN_DHCP_PIDF);
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
