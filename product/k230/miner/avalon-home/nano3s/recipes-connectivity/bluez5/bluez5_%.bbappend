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

# The phone app reads GATT characteristics by index: only GAP Device Name (0x2a00) may
# precede the 0xffe1 provisioning characteristic. DeviceID=false, Channels=1 and the
# patch remove the others (docs/miner/MINER.md → "`:4028` API"). ControllerMode=le: LE
# peripheral only.
# ReverseServiceDiscovery=false: no GATT client on the phone, which would ask the phone for
# pairing the adapter refuses and stall the provisioning reads.
FILESEXTRAPATHS:prepend:k230-nano3s := "${THISDIR}/files:"

SRC_URI:append:k230-nano3s = " \
    file://0001-gatt-database-drop-readable-gap-gatt.patch \
    https://git.openembedded.org/openembedded-core/plain/meta/recipes-connectivity/bluez5/bluez5/CVE-2023-27349.patch?id=adaebd54ea6f53bfbc093c3bdac4f02b0975cb15;downloadfilename=bluez5-CVE-2023-27349.patch;name=cve-2023-27349 \
"
SRC_URI[cve-2023-27349.sha256sum] = "6956a6a23e0f8beb4d4f8cacff2b439fe0b7e76b082183c8f465709accf8c77c"

do_install:append:k230-nano3s() {
    install -d ${D}${sysconfdir}/bluetooth
    {
        printf '[General]\nDeviceID = false\nControllerMode = le\n'
        printf 'ReverseServiceDiscovery = false\n\n[GATT]\nChannels = 1\n'
    } > ${D}${sysconfdir}/bluetooth/main.conf
}

FILES:${PN}:append:k230-nano3s = " ${sysconfdir}/bluetooth/main.conf"

PACKAGECONFIG:remove:k230-nano3s = " \
    readline \
    obex-profiles \
    a2dp-profiles \
    avrcp-profiles \
    network-profiles \
    hid-profiles \
    hog-profiles \
"
