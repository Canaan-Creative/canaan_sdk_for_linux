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

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " \
    file://0001-riscv-k230-add-c908-cpu-support.patch \
    file://0002-riscv-k230-boot-through-spl-opensbi.patch \
    file://0003-riscv-make-reset-cpu-weak.patch \
    file://0004-lib-gunzip-make-gunzip-weak.patch \
    file://0005-dt-bindings-pinctrl-k230-pin-functions.patch \
    file://0006-riscv-dts-k230-add-soc-dtsi.patch \
    file://0007-board-canaan-add-k230-common-board.patch \
    file://0008-riscv-source-k230-common-kconfig.patch \
    file://0009-env-add-spi-nand-backend.patch \
    file://0010-pwm-add-kendryte-pwm-driver.patch \
    file://0100-spi-designware-k230-enhanced-spi.patch \
    file://0101-mtd-build-spi-nand-core-in-spl.patch \
    file://0102-mtd-spinand-gigadevice-add-gd5f4gm8.patch \
    file://0103-mtd-spinand-winbond-add-w25n02kw.patch \
    file://0104-mtd-spinand-keep-unreadable-marker-good.patch \
    file://0200-board-canaan-k230-nano3s-board.patch \
    file://0201-riscv-add-k230-nano3s-target.patch \
    file://firmware_gen.py \
"

inherit python3native
DEPENDS:append = " python3-pycryptodome-native u-boot-tools-native"

K230_RELAX_CFLAGS = " \
    -Wno-error=int-conversion \
    -Wno-error=incompatible-pointer-types \
    -Wno-error=implicit-function-declaration \
    -Wno-error=int-to-pointer-cast \
    -Wno-error=pointer-to-int-cast \
"
K230_AMP_CFLAGS = "${@bb.utils.contains('K230_AMP', '1', ' -DCONFIG_RT_SMART_RUN_CORE_ID=0', \
    '', d)}"
EXTRA_OEMAKE:append = ' KCFLAGS="${K230_RELAX_CFLAGS}${K230_AMP_CFLAGS}"'

def k230_uboot_scmversion(d):
    import subprocess, os
    root = d.getVar('CANAAN_SDK_ROOT')
    if not root or not os.path.isdir(os.path.join(root, '.git')):
        return ''
    paths = [
        'product/k230/common/recipes-bsp/u-boot',
        'build/yocto/meta-canaan/recipes-bsp/u-boot',
    ]
    machine = d.getVar('MACHINE') or ''
    for layer in (d.getVar('BBLAYERS') or '').split():
        rel = os.path.relpath(os.path.realpath(layer), os.path.realpath(root))
        if (rel.startswith('product' + os.sep) and
                os.path.isfile(os.path.join(layer, 'conf', 'machine', machine + '.conf'))):
            paths.append(os.path.join(rel, 'conf', 'machine'))
    try:
        out = subprocess.check_output(
            ['git', '-C', root, 'status', '--porcelain', '--'] + paths,
            stderr=subprocess.DEVNULL)
    except Exception:
        return ''
    return '-dirty' if out.strip() else ''

UBOOT_SCMVERSION = "${@k230_uboot_scmversion(d)}"

do_compile:prepend() {
    unset SOURCE_DATE_EPOCH
    printf '%s' "${UBOOT_SCMVERSION}" > ${S}/.scmversion
}

do_patch[postfuncs] += "k230_restore_script_perms"
k230_restore_script_perms() {
    chmod +x ${S}/arch/riscv/cpu/k230/ddr.sh 2>/dev/null || true
    chmod +x ${S}/arch/riscv/cpu/k230/ddr_2d.sh 2>/dev/null || true
    chmod +x ${S}/tools/endian-swap.py 2>/dev/null || true
    install -m 0755 ${WORKDIR}/firmware_gen.py ${S}/tools/firmware_gen.py
}

do_configure:prepend() {
    if [ "${MACHINE}" = "k230-nano3s" ]; then
        dc="${S}/configs/k230_nano3s_defconfig"
        if [ -f "$dc" ] && ! grep -q '^CONFIG_AUTOBOOT_KEYED=y' "$dc"; then
            printf 'CONFIG_AUTOBOOT_KEYED=y\nCONFIG_AUTOBOOT_STOP_STR=""\n' >> "$dc"
        fi
    fi
}

do_deploy:append() {
    install -d ${DEPLOYDIR}/k230-tools
    if [ -f ${S}/tools/firmware_gen.py ]; then
        install -m 0755 ${S}/tools/firmware_gen.py ${DEPLOYDIR}/k230-tools/firmware_gen.py
    fi
}
