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

#ifndef MC_PATHS_H
#define MC_PATHS_H

#define USERCONFIG_DIR        "/data/userconfig"
#define MC_SYSTEMCFG_DEFAULT  "/data/userconfig/systemcfg.ini"
#define FACTORY_DEFAULT_DIR   "/mntapp/release/linux/confiles/usrcon"
#define MC_SYSTEMCFG_FACTORY  FACTORY_DEFAULT_DIR "/systemcfg.ini"
#define MC_ACTIVATION_DEFAULT "/data/userconfig/activation.ini"
#define MC_ACTIVATION_FACTORY FACTORY_DEFAULT_DIR "/activation.ini"
#define POOL_CONFIG_DEFAULT   "/data/userconfig/pools.conf"
#define MM_CFGWR_WP_DIR       "/data/userconfig/wallpaper"
#define MC_WEB_TOKEN_KEY      "/data/factory/miner-web.tokenkey"
#define WPA_CONF_PATH         "/data/userconfig/wpa_supplicant.conf"
#define WPA_CONF_FACTORY      FACTORY_DEFAULT_DIR "/wpa_supplicant.conf"
#define CALI_INI_PATH         "/data/factory/hashrate_cali.ini"
#define FACTORY_CALI_DIR      "/mntapp/release/linux/confiles/factory"
#define CALI_INI_FACTORY      FACTORY_CALI_DIR "/hashrate_cali.ini"
#define MM_EFUSE_FILE_PATH    "/data/factory/efuse_info.txt"

#endif /* MC_PATHS_H */
