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

# canaan-nano3s.cfg enables the Kconfig-gated board drivers of the common recipe.
CANAAN_NANO3S_FILESDIR := "${THISDIR}/files"
FILESEXTRAPATHS:prepend:k230-nano3s := "${THISDIR}/files:"

LINUX_LOCALVERSION_SUFFIX:k230-nano3s := "-k230-g${@d.getVar('SRCREV')[:12]}"

# Re-trigger do_configure on cfg edits; a varflag takes no override, hence the variable.
CANAAN_NANO3S_CFG_CHECKSUM = ""
CANAAN_NANO3S_CFG_CHECKSUM:k230-nano3s = " \
    ${CANAAN_MINER_KERNEL_CFG}:True \
    ${CANAAN_NANO3S_FILESDIR}/canaan-nano3s.cfg:True \
"
do_configure[file-checksums] += "${CANAAN_NANO3S_CFG_CHECKSUM}"

do_configure:append:k230-nano3s() {
    cat > ${B}/canaan-localversion.cfg <<EOF
CONFIG_LOCALVERSION="${LINUX_LOCALVERSION_SUFFIX}"
EOF
    ${S}/scripts/kconfig/merge_config.sh -m -O ${B} ${B}/.config "${CANAAN_MINER_KERNEL_CFG}" \
        "${CANAAN_NANO3S_FILESDIR}/canaan-nano3s.cfg" ${B}/canaan-localversion.cfg
    yes '' | oe_runmake O=${B} ARCH=${ARCH} olddefconfig
}

KERNEL_DEVICETREE:k230-nano3s = "canaan/k230-nano3s.dtb"
