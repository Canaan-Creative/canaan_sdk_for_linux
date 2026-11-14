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

#ifndef CTRL_ARGS_H
#define CTRL_ARGS_H

#include <stddef.h>
#include <stdint.h>

#define CTRL_IP_STRLEN 16 /* dotted-quad IPv4 text size, NUL included, bytes */

typedef struct mc_ctrl_cmd mc_ctrl_cmd;

typedef struct {
    int  is_static;            /* 1 static, 0 DHCP */
    char ip[CTRL_IP_STRLEN];   /* IPv4 address, dotted quad; "" for DHCP */
    char mask[CTRL_IP_STRLEN]; /* netmask, dotted quad; "" for DHCP */
    char gw[CTRL_IP_STRLEN];   /* gateway, dotted quad; "" for DHCP */
    char dns[CTRL_IP_STRLEN];  /* DNS server, dotted quad; "" if not given */
} ctrl_ip_t;

int  ctrl_parse_lcd(const char *args, uint16_t *id, uint16_t *value);
int  ctrl_parse_workmode(const char *args, uint8_t *mode);
int  ctrl_parse_worklevel(const char *args, int *level);
int  ctrl_parse_mode_level(const char *args, uint8_t *mode, int *level);
int  ctrl_parse_ledmode(const char *args, uint8_t *mode);
int  ctrl_parse_ledset(const char *args, uint8_t *effect, uint8_t *bright, uint32_t *temper,
         uint32_t *rgb);
int  ctrl_parse_nightlamp(const char *args, uint8_t *mode, uint32_t *duration_s);
int  ctrl_parse_ip(const char *args, ctrl_ip_t *out);
int  ctrl_ipv4_parse(const char *s, uint32_t *out);
void ctrl_ipv4_fmt(uint32_t v, char *out, size_t cap);
int  ctrl_scan_dec(const char **pp, uint32_t max, uint32_t *out);
int  ctrl_scan_int(const char **pp, int *out);
int  ctrl_scan_int_saturating(const char **pp, int *out);
int  ctrl_parse_cmd(uint16_t op, const char *args, mc_ctrl_cmd *out);

#endif /* CTRL_ARGS_H */
