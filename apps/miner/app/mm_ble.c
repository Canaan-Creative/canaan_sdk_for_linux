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

#include "mm_ble.h"

#ifdef SUPPORT_BLE
#include "osif.h"

#include "wifi_mgr.h"

void net_provision_start(char *name)
{
    if (wifi_init(name) != 0) {
        osif_log(OSIF_LOG_ERR, "BLE setup '%s' failed to start", name);
        return;
    }
    osif_log(OSIF_LOG_INFO, "Bluetooth name '%s'", name);
    wifi_startup();
}
#endif /* SUPPORT_BLE */
