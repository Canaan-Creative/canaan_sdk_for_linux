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

DESCRIPTION = "Avalon Nano3s miner image."
LICENSE = "MIT"

IMAGE_INSTALL = "packagegroup-core-boot avalon-miner"
# Web UI on port 443 (HTTPS), REST API on port 8443 (HTTPS).
IMAGE_INSTALL += " miner-web "

IMAGE_INSTALL += " kmod procps os-release "
IMAGE_INSTALL += " chrony "
# glibc dlopen()s libgcc_s for pthread_exit and backtrace.
IMAGE_INSTALL += " libgcc "
# Shared zlib and OpenSSL libraries ship unconditionally.
IMAGE_INSTALL += " zlib libssl "
# Framebuffer tests for the ST7789V SPI panel on /dev/fb0.
IMAGE_INSTALL += " fb-test "
# RTL8723DU WiFi; the module comes through the rtl8723du package so bitbake builds that recipe.
IMAGE_INSTALL += " rtl8723du wpa-supplicant wpa-supplicant-cli "
# RTL8723DU Bluetooth firmware and the init script that rebinds btusb to load it.
IMAGE_INSTALL += " rtl8723d-bt-fw "
# Time zones for mm_miner; the tzdata meta package would add the duplicate posix and right trees.
IMAGE_INSTALL += " tzdata-core tzdata-misc tzdata-africa tzdata-americas "
IMAGE_INSTALL += " tzdata-antarctica tzdata-arctic tzdata-asia tzdata-atlantic "
IMAGE_INSTALL += " tzdata-australia tzdata-europe tzdata-pacific "
# System bus for bluetoothd and the BLE client, listed for the image manifest.
IMAGE_INSTALL += " dbus "

# Slimming for the 48 MiB rootfs_a; the deb backend honours PACKAGE_EXCLUDE, not
# BAD_RECOMMENDATIONS.
IMAGE_LINGUAS = ""
PACKAGE_EXCLUDE += " shared-mime-info "

# SSH server.
IMAGE_FEATURES:append = " ssh-server-openssh "

# /tmp must be tmpfs: the OTA success latch /tmp/up_scceed_file clears only on reboot.
ROOTFS_POSTPROCESS_COMMAND += "k230_nano3s_add_tmpfs_tmp; "

k230_nano3s_add_tmpfs_tmp() {
    cat >> ${IMAGE_ROOTFS}/etc/fstab <<EOF
tmpfs                /tmp                 tmpfs      mode=1777             0  0
EOF
}

export IMAGE_BASENAME = "nano3s-image-full"

# Writes /etc/release_version and prints it at boot.
inherit canaan-release-version
CANAAN_RELEASE_MINER_VERSION = "${CANAAN_MINER_VERSION}"

inherit canaan-boot-licenses
inherit core-image
