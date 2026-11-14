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

ROOTFS_POSTPROCESS_COMMAND += "canaan_copy_boot_licenses; "
do_rootfs[depends] += "opensbi:do_populate_lic u-boot-k230:do_populate_lic"

canaan_copy_boot_licenses() {
    for pn in opensbi u-boot-k230; do
        install -d ${IMAGE_ROOTFS}${datadir}/common-licenses/$pn
        cp -R ${LICENSE_DIRECTORY}/$pn/. ${IMAGE_ROOTFS}${datadir}/common-licenses/$pn/
    done
}
