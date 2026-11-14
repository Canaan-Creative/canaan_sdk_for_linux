# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Canaan Creative Co., Ltd.
#
# For the licensing terms, see the LICENSE file (MIT License).
#
# NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability, fitness for a
# particular purpose and non-infringement. In no event shall the authors or copyright
# holders be liable for any claim, damages or other liability arising from, out of or in
# connection with the Software or the use or other dealings in the Software.

# ssl: HTTPS with the OpenSSL 3 API, linked at build time.
# caching stays off: rootfs files share one mtime, so browsers would keep stale pages after an OTA.
PACKAGECONFIG = "ipv6 ssl"
PACKAGECONFIG[ssl] = " \
    -DCIVETWEB_ENABLE_SSL=ON -DCIVETWEB_SSL_OPENSSL_API_1_1=OFF \
    -DCIVETWEB_SSL_OPENSSL_API_3_0=ON -DCIVETWEB_ENABLE_SSL_DYNAMIC_LOADING=OFF, \
    -DCIVETWEB_ENABLE_SSL=OFF, \
    openssl \
"

SRCREV = "d110e8731c6aa0fcd04504deb449bdb773cfb9da"
PV = "1.16+git${SRCPV}"
LIC_FILES_CHKSUM = "file://LICENSE.md;md5=e5f28949b2d9ec1f4da8bb00aff8b6d4"
