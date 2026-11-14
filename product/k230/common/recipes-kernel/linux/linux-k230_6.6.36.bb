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

SUMMARY = "K230 Linux 6.6 kernel (ruyisdk/linux-xuantie-kernel + K230 patches)"

inherit kernel
require recipes-kernel/linux/linux-yocto.inc

LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

KERNEL_DEBUG_TIMESTAMPS = "1"

SRC_URI = "git://github.com/ruyisdk/linux-xuantie-kernel.git;branch=linux-6.6.36;nobranch=1;protocol=https;destsuffix=linux-src \
    file://0001-dt-bindings-pinctrl-k230-iomux.patch \
    file://0002-pinctrl-canaan-k230-iomux-driver.patch \
    file://0003-pinctrl-build-k230-iomux-driver.patch \
    file://0004-canaan-hwtimer-k230-timer-driver.patch \
    file://0005-drivers-build-canaan-hwtimer.patch \
    file://0006-canaan-ws2812-k230-i2s-led-driver.patch \
    file://0007-drivers-build-canaan-ws2812.patch \
    file://0008-canaan-hardlock-k230-spinlock-driver.patch \
    file://0009-drivers-build-canaan-hardlock.patch \
    file://0010-spi-dw-k230-enhanced-definitions.patch \
    file://0011-spi-dw-k230-enhanced-driver.patch \
    file://0012-spi-build-k230-enhanced-driver.patch \
    file://0013-riscv-dts-k230-iomux-controllers.patch \
    file://0014-riscv-dts-k230-ai-clocks.patch \
    file://0015-riscv-dts-k230-nonai2d-gdma-nodes.patch \
    file://0016-riscv-dts-k230-sdhci0-emmc-clock.patch \
    file://0017-riscv-dts-k230-pwm-node-names.patch \
    file://0018-riscv-dts-k230-per-pin-gpio-provider.patch \
    file://0019-riscv-dts-k230-use-per-pin-gpio.patch \
    file://0020-riscv-dts-k230-hwtimer-nodes.patch \
    file://0021-riscv-dts-k230-watchdog-wdt0.patch \
    file://0022-riscv-dts-k230-shared-memory-layout.patch \
    file://0023-riscv-k230-defconfig-update.patch \
    file://0024-crypto-canaan-k230-puf-dma-lock.patch \
    file://0025-crypto-build-k230-puf-dma-lock.patch \
    file://0026-soc-canaan-k230-pmu-power-key-driver.patch \
    file://0027-soc-canaan-build-k230-pmu-driver.patch \
    file://0028-riscv-dts-k230-pmu-node.patch \
    file://0100-riscv-sparsemem-physmem-bits-32.patch \
    file://0101-clk-k230-keep-gate-no-reparent.patch \
    file://0102-soc-canaan-k230-drop-hardlock-props.patch \
    file://0103-misc-canaan-ai-power-and-clocks.patch \
    file://0104-drm-bridge-lt9611-k230-display.patch \
    file://0105-drm-canaan-reset-display-on-enable.patch \
    file://0106-mmc-kendryte-tuning-loop-count.patch \
    file://0107-r8152-k230-trng-mac-address.patch \
    file://0108-asoc-inno-k230-digital-mute.patch \
    file://0109-gpio-k230-per-pin-definitions.patch \
    file://0110-gpio-k230-per-pin-driver.patch \
    file://0111-pwm-k230-normal-polarity-clean-stop.patch \
    file://0112-dma-k230-peridma-llt-and-null-desc.patch \
    file://0113-dma-k230-peridma-burst-from-maxburst.patch \
    file://0114-dma-k230-peridma-w1c-clear-per-channel.patch \
    file://0115-serial-8250-tx-only-dma-shadow-thr.patch \
    file://0116-spi-dw-quad-dual-read.patch \
    file://0117-spi-dw-bound-rx-drain-quiet-errors.patch \
    file://0118-spi-dw-mmio-k230-optional-irqs.patch \
    file://0119-mtd-spinand-winbond-w25n02kw.patch \
    file://0120-iio-adc-k230-fixed-delay.patch \
    file://0121-staging-fbtft-keep-boot-logo.patch \
    file://0122-input-pwm-beeper-snd-volume.patch \
    file://0123-watchdog-dw-wdt-pretimeout-sigusr2.patch \
    file://0124-watchdog-dw-wdt-bootstatus.patch \
    file://0125-rtc-k230-day-field-five-bits.patch \
    file://0126-watchdog-dw-wdt-panic-record.patch \
    file://0127-crypto-kendryte-hash-data-path.patch \
    file://0128-crypto-kendryte-aes-puf-dma-lock.patch \
    file://0129-reset-k230-map-restart-register-at-probe.patch \
    file://0200-riscv-dts-k230-add-nano3s-board.patch \
    file://0201-riscv-dts-canaan-build-nano3s-dtb.patch \
"

SRCREV = "7d4e1f444f461dbe3833bd99a4640e7b6c2cd529"
LINUX_VERSION = "6.6.36"
LINUX_VERSION_EXTENSION = "-k230"
LINUX_KERNEL_TYPE = "standard"
PV = "${LINUX_VERSION}+git${SRCPV}"

PREMIRRORS:prepend = "git://github.com/ruyisdk/linux-xuantie-kernel.git \
    git://${CANAAN_SDK_ROOT}/os/linux;protocol=file \n"

S = "${WORKDIR}/linux-src"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

DEPENDS += "pahole-native elfutils-native"
KERNEL_VERSION_SANITY_SKIP = "1"
KCONFIG_MODE = "--alldefconfig"

COMPATIBLE_MACHINE = "k230-.*"
PACKAGE_ARCH = "${MACHINE_ARCH}"
