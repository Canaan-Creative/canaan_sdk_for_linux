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

SUMMARY = "Filesystem and flash image generator with the K230 kdimage type"
DESCRIPTION = "genimage 18 with the kdimage (.kdimg) and uffs image types."
HOMEPAGE = "https://github.com/kendryte/canmv_k230/tree/main/tools/genimage"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=b234ee4d69f5fce4486a80fdaf4a4263"

SRC_URI = "git://github.com/kendryte/canmv_k230.git;protocol=https;branch=main"
SRCREV = "c05f7f56e7d634760d0ae8c57a7d81607f8e4823"
PV = "18+git${SRCPV}"
S = "${WORKDIR}/git/tools/genimage"

DEPENDS = "libconfuse-native openssl-native"

inherit autotools pkgconfig native
