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

SUMMARY = "Realtek RTL8723DU USB WiFi driver"
DESCRIPTION = "Out-of-tree kernel module for the Realtek RTL8723DU USB WiFi adapter."

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = " \
    file://os_dep/os_intfs.c;beginline=1;endline=2;md5=c4e219747f5afa6255341b03b4b590ef \
"

inherit module

SRC_URI = " \
    git://github.com/lwfinger/rtl8723du.git;protocol=https;branch=master \
    file://0001-rtl8723du-out-of-tree-pmkid-scan-fixes.patch \
"
SRCREV = "ae03b0861351f72808405ddda80e8aab105bcfce"
S = "${WORKDIR}/git"

# Build against the staged target kernel.
EXTRA_OEMAKE:append = " CONFIG_RTL8723DU=m KSRC=${STAGING_KERNEL_DIR} KVER=${KERNEL_VERSION}"

# Power saving off: with IPS/LPS enabled a connect from idle fails.
KERNEL_MODULE_PROBECONF += "8723du"
module_conf_8723du = "options 8723du rtw_ips_mode=0 rtw_power_mgnt=0"

# The vendor Makefile only builds the module; install it here.
do_install() {
    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra
    install -m 0644 ${S}/8723du.ko \
        ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra/8723du.ko
}
