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

SUMMARY = "U-Boot 2025.07 for K230"
DESCRIPTION = "U-Boot 2025.07 with the K230 SPL and U-Boot images wrapped in the K230"
DESCRIPTION += "firmware header."

LICENSE = "GPL-2.0-or-later & GPL-2.0-only & BSD-2-Clause"
LIC_FILES_CHKSUM = " \
    file://Licenses/README;md5=2ca5f2c35c8cc335f0a19756634782f1 \
    file://Licenses/bsd-2-clause.txt;md5=6a31f076f5773aabd8ff86191ad6fdd5 \
    file://cmd/elf.c;beginline=1;endline=5;md5=3e65d352f169d51b56c3e10f01bd7ef3 \
"

PROVIDES = "virtual/bootloader u-boot"
PE = "1"

inherit deploy

SRC_URI = "git://${CANAAN_SDK_ROOT}/bootloader/u-boot;protocol=file;branch=master;nobranch=1"
SRCREV = "e37de002fac3895e8d0b60ae2015e17bb33e2b5b"

S = "${WORKDIR}/git"
# add_firmware_head uses paths relative to the source tree.
B = "${S}"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

DEPENDS = " \
    flex-native \
    bison-native \
    bc-native \
    dtc-native \
    swig-native \
    openssl-native \
    python3-native \
    python3-setuptools-native \
    python3-pyelftools-native \
    opensbi \
"

do_configure[depends] += "opensbi:do_deploy"

# Each machine conf sets its board defconfig.
UBOOT_DEFCONFIG ?= "k230_nano3s_defconfig"

EXTRA_OEMAKE = ' \
    CROSS_COMPILE=${TARGET_PREFIX} \
    ARCH=riscv \
    OPENSBI=${DEPLOY_DIR_IMAGE}/fw_dynamic.bin \
    HOSTCC="${BUILD_CC} ${BUILD_CFLAGS} ${BUILD_LDFLAGS}" \
    STAGING_INCDIR=${STAGING_INCDIR_NATIVE} \
    STAGING_LIBDIR=${STAGING_LIBDIR_NATIVE} \
    V=1 \
'

do_configure() {
    oe_runmake -C ${S} ${UBOOT_DEFCONFIG}
}

do_compile() {
    # Keep target flags out of the host tool builds.
    unset LDFLAGS CFLAGS CPPFLAGS
    oe_runmake -C ${S}
    oe_runmake -C ${S} add_firmware_head
}

do_install() {
    install -d ${D}/boot
    install -m 0644 ${S}/u-boot.bin ${D}/boot/u-boot.bin
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${S}/u-boot.bin ${DEPLOYDIR}/u-boot.bin
    install -m 0644 ${S}/spl/u-boot-spl.bin ${DEPLOYDIR}/u-boot-spl.bin
    # Images with the K230 firmware header.
    install -m 0644 ${S}/u-boot-spl-k230.bin ${DEPLOYDIR}/fn_u-boot-spl.bin
    install -m 0644 ${S}/u-boot-spl-k230-swap.bin ${DEPLOYDIR}/swap_fn_u-boot-spl.bin
    install -m 0644 ${S}/fn_u-boot.img ${DEPLOYDIR}/fn_ug_u-boot.bin
}
addtask deploy before do_build after do_install

FILES:${PN} = "/boot/u-boot.bin"

COMPATIBLE_MACHINE = "k230-.*"
PACKAGE_ARCH = "${MACHINE_ARCH}"
