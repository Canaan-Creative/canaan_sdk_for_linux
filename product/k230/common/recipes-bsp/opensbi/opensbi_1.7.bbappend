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

# Build only from the in-repo bootloader/opensbi submodule.
SRC_URI:remove = "git://github.com/XUANTIE-RV/opensbi.git;branch=opensbi-v1.7-dev;protocol=https"
SRC_URI:remove = "${XUANTIE_GIT}/opensbi.git;branch=opensbi-v1.7-dev;protocol=https"
SRC_URI:append = " \
    git://${CANAAN_SDK_ROOT}/bootloader/opensbi;protocol=file;branch=master;nobranch=1 \
    file://0001-thead-clear-c908-maee-for-k230.patch \
"
# Matches the bootloader/opensbi submodule gitlink.
SRCREV = "ec52ac7d13cb477d3adf70b46a306c817a5b5603"

LICENSE = "BSD-2-Clause & BSD-3-Clause & GPL-2.0-only"
LIC_FILES_CHKSUM = " \
    file://COPYING.BSD;md5=42dd9555eb177f35150cf9aa240b61e5 \
    file://lib/utils/sys/htif.c;beginline=2;endline=2;md5=b23faac1da952fa6845fd4fb63a20ffe \
    file://lib/utils/sys/atcsmu.c;beginline=2;endline=2;md5=b23faac1da952fa6845fd4fb63a20ffe \
    file://platform/generic/andes/andes_pma.c;beginline=1;endline=1;md5=50d2ba0afecd20f74c12a4bdbcfcfe61 \
    file://platform/generic/renesas/rzfive/rzfive.c;beginline=1;endline=1;md5=50d2ba0afecd20f74c12a4bdbcfcfe61 \
"

# fw_payload embeds the machine's kernel and memory map, so the artifacts are per machine.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# Board bbappends that embed Linux in fw_payload override this.
do_build_fwpayload() {
    :
}
addtask build_fwpayload after do_compile before do_deploy
