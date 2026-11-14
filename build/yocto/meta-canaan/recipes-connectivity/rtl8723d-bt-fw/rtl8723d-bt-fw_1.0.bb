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

SUMMARY = "Realtek RTL8723D Bluetooth firmware"
DESCRIPTION = "Bluetooth controller firmware for the RTL8723DU, loaded by the in-kernel btrtl"
DESCRIPTION += "driver."

LICENSE = "Firmware-rtlwifi_firmware & MIT"
RTL_BT_LICENCE = "${WORKDIR}/linux-firmware-20261019-LICENCE.rtlwifi_firmware.txt"
LIC_FILES_CHKSUM = " \
    file://${RTL_BT_LICENCE};md5=00d06cfd3eddd5a2698948ead2ad54a5 \
    file://${CANAAN_SDK_ROOT}/LICENSE;beginline=1;endline=18;md5=255f95baaf5ed8b6a4a1b241f082b682 \
"
NO_GENERIC_LICENSE[Firmware-rtlwifi_firmware] = "${RTL_BT_LICENCE}"

LINUX_FIRMWARE_REV = "a2dcb924f0897368b0b4f305cbf9808cdda90ce0"
LINUX_FIRMWARE_URI = "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"

SRC_URI = " \
    ${LINUX_FIRMWARE_URI}/rtl_bt/rtl8723d_fw.bin?id=${LINUX_FIRMWARE_REV};downloadfilename=linux-firmware-20261019-rtl8723d_fw.bin;name=fw \
    ${LINUX_FIRMWARE_URI}/rtl_bt/rtl8821c_config.bin?id=${LINUX_FIRMWARE_REV};downloadfilename=linux-firmware-20261019-rtl8821c_config.bin;name=config \
    ${LINUX_FIRMWARE_URI}/LICENSES/LICENCE.rtlwifi_firmware.txt?id=${LINUX_FIRMWARE_REV};downloadfilename=linux-firmware-20261019-LICENCE.rtlwifi_firmware.txt;name=licence \
    file://rtl8723d-bt-reprobe \
"
SRC_URI[fw.sha256sum] = "6b70723b2db9b658cd9535fe36f1494b4494699e1ad07920c9356f8bd90d9263"
SRC_URI[config.sha256sum] = "292717b0610ce920f66b734c299d349ea59a4c4400044078d8fc65625f17e382"
SRC_URI[licence.sha256sum] = "a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e"

inherit allarch update-rc.d

RDEPENDS:${PN} = "bluez5"

# btusb probes before the rootfs is mounted; rtl8723d-bt-reprobe rebinds it
# so btrtl loads this firmware.
INITSCRIPT_NAME = "rtl8723d-bt-reprobe"
INITSCRIPT_PARAMS = "start 19 2 3 4 5 ."

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/rtl_bt
    install -m 0644 ${WORKDIR}/linux-firmware-20261019-rtl8723d_fw.bin \
        ${D}${nonarch_base_libdir}/firmware/rtl_bt/rtl8723d_fw.bin
    install -m 0644 ${WORKDIR}/linux-firmware-20261019-rtl8821c_config.bin \
        ${D}${nonarch_base_libdir}/firmware/rtl_bt/rtl8723d_config.bin
    install -m 0644 ${WORKDIR}/linux-firmware-20261019-LICENCE.rtlwifi_firmware.txt \
        ${D}${nonarch_base_libdir}/firmware/LICENCE.rtlwifi_firmware.txt
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/rtl8723d-bt-reprobe ${D}${sysconfdir}/init.d/rtl8723d-bt-reprobe
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/firmware/rtl_bt/rtl8723d_fw.bin \
    ${nonarch_base_libdir}/firmware/rtl_bt/rtl8723d_config.bin \
    ${nonarch_base_libdir}/firmware/LICENCE.rtlwifi_firmware.txt \
    ${sysconfdir}/init.d/rtl8723d-bt-reprobe \
"
