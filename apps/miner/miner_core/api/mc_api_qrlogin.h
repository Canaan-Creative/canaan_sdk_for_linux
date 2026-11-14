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

#ifndef MC_API_QRLOGIN_H
#define MC_API_QRLOGIN_H

int  mc_api_qr_login_begin(const char *token);
int  mc_api_qr_login_pending(const char *nonce);
int  mc_api_qr_login_approve(const char *nonce);
int  mc_api_qr_login_take(const char *token);
void mc_api_set_peer_trusted(int trusted);
int  mc_api_peer_trusted(void);

#endif /* MC_API_QRLOGIN_H */
