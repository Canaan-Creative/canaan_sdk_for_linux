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

# fw_payload boot model: the Linux Image is embedded in the OpenSBI blob.

# OpenSBI's Makefile needs B separate from S for O=${B}.
B:k230-nano3s = "${WORKDIR}/build"

# OpenSBI builds only without a parallel make jobserver.
EXTRA_OEMAKE:k230-nano3s = "-j1"
do_compile:prepend:k230-nano3s() {
    unset MAKEFLAGS MFLAGS GMAKEFLAGS
    unset MAKE_JOBSERVER_AUTH MAKEFLAGS_NEW
}

# fw_payload embeds the deployed Linux Image.
do_build_fwpayload[depends] += "${@bb.utils.contains('MACHINE', 'k230-nano3s', \
    'virtual/kernel:do_deploy', '', d)}"

# fw_payload load and entry address.
K230_NANO3S_FW_TEXT_START ?= "0x01000000"

do_build_fwpayload:k230-nano3s() {
    unset MAKEFLAGS MFLAGS GMAKEFLAGS MAKE_JOBSERVER_AUTH

    image_bin="${DEPLOY_DIR_IMAGE}/Image"
    [ -f "$image_bin" ] || bbfatal "fw_payload needs Linux $image_bin (deployed by virtual/kernel)."

    # Addresses match U-Boot k230_boot. FW_OPTIONS=0x1 drops the OpenSBI boot banner; errors
    # still print.
    mkdir -p ${B}
    rm -f ${B}/platform/generic/firmware/fw_payload.*
    oe_runmake -C ${S} O=${B} PLATFORM=generic \
        FW_PAYLOAD=y FW_JUMP=y FW_DYNAMIC=y \
        FW_PAYLOAD_PATH="$image_bin" \
        FW_TEXT_START="${K230_NANO3S_FW_TEXT_START}" \
        FW_PAYLOAD_OFFSET=0x200000 \
        FW_PAYLOAD_FDT_OFFSET=0x2000000 \
        FW_JUMP_ADDR=0x01200000 \
        PLATFORM_RISCV_ISA=rv64gc_xtheadc \
        KCFLAGS="-fno-stack-protector" \
        FW_OPTIONS=0x1 \
        all
}

do_deploy:k230-nano3s() {
    install -m 0755 ${B}/platform/generic/firmware/fw_dynamic.bin ${DEPLOYDIR}
    install -m 0755 ${B}/platform/generic/firmware/fw_dynamic.elf ${DEPLOYDIR}
    install -m 0755 ${B}/platform/generic/firmware/fw_jump.bin ${DEPLOYDIR}
    install -m 0755 ${B}/platform/generic/firmware/fw_jump.elf ${DEPLOYDIR}
    install -m 0755 ${B}/platform/generic/firmware/fw_payload.bin ${DEPLOYDIR}/fw_payload.bin
    install -m 0755 ${B}/platform/generic/firmware/fw_payload.elf ${DEPLOYDIR}/fw_payload.elf
    install -d ${DEPLOYDIR}/k230-tools
    echo "${K230_NANO3S_FW_TEXT_START}" > ${DEPLOYDIR}/k230-tools/FW_TEXT_START
}
