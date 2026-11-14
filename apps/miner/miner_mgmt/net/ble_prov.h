/* SPDX-License-Identifier: MIT */
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

#ifndef BLE_PROV_H
#define BLE_PROV_H

#define BLE_NAME_LEN        16   /* advertised device name buffer, bytes incl. NUL */
#define BLE_DATA_RD_LEN     512  /* read characteristic value buffer, bytes incl. NUL */
#define BLE_DATA_WR_LEN     1024 /* write characteristic reassembly buffer, bytes incl. NUL */
#define BLE_DATA_STATUS_LEN 512  /* status characteristic value buffer, bytes incl. NUL */

/* Handler for one reassembled write-characteristic command string, NULL for an overflow */
typedef int (*ble_data_process_func)(char *);

void ble_dev_init(char *name, ble_data_process_func data_process_cb);
int  ble_dev_startup(void);
void ble_dev_set_power(int onoff);
int  ble_dev_is_run(void);
void ble_set_data_status(char *sta);
void ble_set_data(char *data);

#endif /* BLE_PROV_H */
