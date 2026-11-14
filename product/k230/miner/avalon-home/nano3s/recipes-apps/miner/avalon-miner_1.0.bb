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

SUMMARY = "Avalon Nano3s miner userspace"
DESCRIPTION = "Builds asic_miner, mm_miner and core_miner from apps/miner and installs them"
DESCRIPTION += "as services."
HOMEPAGE = "https://www.canaan.io"

LICENSE = "MIT & OFL-1.1"
LIC_FILES_CHKSUM = " \
    file://${CANAAN_SDK_ROOT}/NOTICE;beginline=127;endline=136;md5=5e42c580f23072b38d68a89a7ce267db \
    file://${CANAAN_SDK_ROOT}/third-party-license/OFL-1.1.txt;md5=f86799f77eaad418e28f2700062fc515 \
    file://${CANAAN_SDK_ROOT}/LICENSE;beginline=1;endline=18;md5=255f95baaf5ed8b6a4a1b241f082b682 \
"

inherit externalsrc pkgconfig
EXTERNALSRC = "${CANAAN_SDK_ROOT}/apps/miner"
EXTERNALSRC_BUILD = "${WORKDIR}/build"

DEPENDS = "libubootenv glib-2.0 openssl lvgl"
RDEPENDS:${PN} = "libubootenv libubootenv-bin bluez5 dbus libcrypto lvgl mtd-utils-ubifs"

CANAAN_FW_TYPE ?= "Debug"
# Build revision, two decimal digits.
MINER_BUILD_REV ?= "01"
# Build time as YYYYMMDDHHmm; empty lets CMake use the current time.
MINER_BUILD_TIME ?= ""
MINER_BUILD_TIME_OPT = "${@('-DMINER_BUILD_TIME=' + d.getVar('MINER_BUILD_TIME')) \
    if d.getVar('MINER_BUILD_TIME') else ''}"

def miner_sw_version(d):
    import os
    root = d.getVar('CANAAN_SDK_ROOT') or ''
    try:
        with open(os.path.join(root, 'apps/miner/VERSION')) as f:
            v = f.read().strip()
            if v:
                return v
    except OSError:
        pass
    v = d.getVar('CANAAN_SDK_VERSION')
    if v:
        return v
    with open(os.path.join(root, 'VERSION')) as f:
        return f.read().strip()
FW_VERSION ?= "${@miner_sw_version(d)}"

inherit cmake
do_configure[noexec] = "1"

inherit python3native

do_compile[nostamp] = "1"

do_compile() {
    unset SOURCE_DATE_EPOCH
    cmake_do_generate_toolchain_file
    TC="-DCMAKE_TOOLCHAIN_FILE=${WORKDIR}/toolchain.cmake"
    BOARD_DESC="${THISDIR}/../../conf/product/boardconf.ini"

    cd ${WORKDIR}
    rm -rf ${B}
    mkdir -p ${B}

    cmake ${S} -B ${B} ${TC} \
        -DFW_TYPE="${CANAAN_FW_TYPE}" -DFW_VERSION=${FW_VERSION} \
        -DMINER_BUILD_REV=${MINER_BUILD_REV} ${MINER_BUILD_TIME_OPT} \
        -DBOARD_DESC=${BOARD_DESC} ${EXTRA_OECMAKE}
    cmake --build ${B} -j${BB_NUMBER_THREADS}
}

APPROOT = "/mntapp/release"
K230_KERNEL_PATCH_DIR = "${CANAAN_SDK_ROOT}/product/k230/common/recipes-kernel/linux/files"
BOARD_DTS_PATCH = "${K230_KERNEL_PATCH_DIR}/0200-riscv-dts-k230-add-nano3s-board.patch"
MINER_DEFAULTS_DIR = "${THISDIR}/../../files"

do_install() {
    PFILES="${THISDIR}/../../files"
    PSVC="${THISDIR}/../../services"

    install -d ${D}${APPROOT}/linux/app
    install -m 0755 ${B}/mm_miner ${D}${APPROOT}/linux/app/
    install -m 0755 ${B}/core_miner ${D}${APPROOT}/linux/app/

    install -d ${D}${APPROOT}/linux/resource/assets_lcd
    cp -r ${PFILES}/lcd_img/* ${D}${APPROOT}/linux/resource/assets_lcd/

    install -d ${D}${APPROOT}/asic
    install -m 0755 ${B}/asic_miner ${D}${APPROOT}/asic/

    install -d ${D}/data
    install -d ${D}${APPROOT}/linux/confiles/usrcon ${D}${APPROOT}/linux/confiles/factory
    install -m 0644 ${MINER_DEFAULTS_DIR}/usrcon/* ${D}${APPROOT}/linux/confiles/usrcon/
    install -m 0644 ${MINER_DEFAULTS_DIR}/factory/* ${D}${APPROOT}/linux/confiles/factory/

    install -d ${D}${sysconfdir}
    install -m 0755 ${PFILES}/etc/wpa_action.sh ${D}${sysconfdir}/wpa_action.sh

    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${PSVC}/avalon-miner.init ${D}${sysconfdir}/init.d/avalon-miner

    install -d ${D}${sbindir}
    install -m 0755 ${PSVC}/avalon-app-mount.sh ${D}${sbindir}/avalon-app-mount.sh
    dts="${BOARD_DTS_PATCH}"
    [ -f "$dts" ] || bbfatal "fw_env.config: board DTS not found: $dts"
    labels=$(sed -n '/partitions {/,/^+[[:space:]]*};/p' "$dts" |
        sed -n 's/^+.*label[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p')
    env_a=$(printf '%s\n' "$labels" | grep -n '^uboot_env_a$' | cut -d: -f1 || true)
    env_b=$(printf '%s\n' "$labels" | grep -n '^uboot_env_b$' | cut -d: -f1 || true)
    [ -n "$env_a" ] && [ -n "$env_b" ] ||
        bbfatal "fw_env.config: no uboot_env_a/uboot_env_b partition in $dts"
    {
        printf '# MTD device name       Device offset   Env. size       Flash sector size\n'
        printf '%-23s %-11s %-16s %s\n' "/dev/mtd$(expr $env_a - 1)" 0x0 0x10000 0x20000
        printf '%-23s %-11s %-16s %s\n' "/dev/mtd$(expr $env_b - 1)" 0x0 0x10000 0x20000
    } > ${D}${sysconfdir}/fw_env.config
    chmod 0644 ${D}${sysconfdir}/fw_env.config

    install -m 0755 ${CANAAN_SDK_ROOT}/product/k230/miner/common/services/avalon-data-mount.sh \
        ${D}${sbindir}/avalon-data-mount.sh
}

do_install[file-checksums] += "${BOARD_DTS_PATCH}:True"

PACKAGES =+ "${PN}-core ${PN}-config"
FILES:${PN}-core = "${APPROOT}/linux/app ${APPROOT}/asic/asic_miner"
FILES:${PN}-config = "${APPROOT}/linux/resource ${APPROOT}/linux/confiles"
RDEPENDS:${PN}-config = "${PN}-core"
FILES:${PN} += "/data ${sbindir}/avalon-app-mount.sh ${sbindir}/avalon-data-mount.sh"
FILES:${PN} += "${sysconfdir}/fw_env.config"

inherit update-rc.d
INITSCRIPT_NAME = "avalon-miner"
INITSCRIPT_PARAMS = "defaults 90 10"
