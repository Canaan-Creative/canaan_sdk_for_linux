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

#include "net_ident.h"
#include "net_linux_paths.h"

#include <stdio.h>

int mm_mac_tail(const char *ifname, char *out, size_t n)
{
    char     path[256];
    unsigned m[6];
    FILE    *f;
    int      got;

    snprintf(path, sizeof path, MM_SYS_NET_DIR "%s/address", ifname);
    f = fopen(path, "re");
    if (!f)
        return -1;
    got = fscanf(f, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6;
    fclose(f);
    if (!got || !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]))
        return -1;
    snprintf(out, n, "%02x%02x", m[4], m[5]);
    return 0;
}
