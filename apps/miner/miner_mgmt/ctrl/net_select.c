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

#include "net_select.h"

net_select_action net_select_eval(net_select_t *st, int link, int client)
{
    if (!st)
        return NET_SEL_NONE;

    st->link   = link ? 1 : 0;
    st->client = client ? 1 : 0;

    if (st->link && !st->client)
        return NET_SEL_DHCP_START;
    if (!st->link && st->client)
        return NET_SEL_DHCP_STOP;
    return NET_SEL_NONE;
}

int net_select_wired_up(const net_select_t *st)
{
    return st && st->link;
}
