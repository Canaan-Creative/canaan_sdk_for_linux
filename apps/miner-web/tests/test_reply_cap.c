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

#include <stdio.h>

#include "telemetry.h"
#include "mc_api_types.h"

_Static_assert(MW_RPC_REPLY_CAP >= MC_API_REPLY_CAP,
        "miner_web truncates the largest core API reply this board's core can send");

int main(void)
{
    printf("reply_cap: %u >= %u\n", MW_RPC_REPLY_CAP, (unsigned)MC_API_REPLY_CAP);
    return 0;
}
