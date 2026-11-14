#!/bin/sh
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

# Attach the miner app partition as ubi1 and mount its UBIFS volume at
# /mntapp/release. Run by the avalon-miner init script before the miner starts.
#
# The A/B slot comes from the U-Boot env variable app_partition, read with
# fw_printenv. The MTD number of app_a or app_b is looked up by partition label
# in /proc/mtd, never hardcoded: MTD indices follow device-tree partition order
# and nothing pins them, so mm_ota also resolves slots by name. rootfs is ubi0.
#
# Re-running applies the current app_partition. If ubi1 is attached to the other
# slot, for example after an OTA flip followed by a miner restart without a
# reboot, it is detached and the selected slot attached. Attach and mount
# failures abort under set -e, and the init script then refuses to start the
# miner.
set -e

MOUNTPOINT=/mntapp/release
UBIDEV=1
VOL=ubi_app_part

slot=$(fw_printenv -n app_partition 2>/dev/null) || slot=""
if [ -z "$slot" ]; then
    # Report the fallback, or a half-applied OTA (kernel and rootfs on B, app
    # left on A) looks like a normal boot. Slot A is still the only safe
    # choice: a unit whose env was never seeded has only ever run slot A, and
    # refusing to mount would leave it unable to start.
    echo "avalon-app-mount: active application slot not recorded in the boot settings;" \
        "using slot A" >&2
    slot=a
fi

# U-Boot selects the slot on the first character only ('b' or 'B' means slot B)
# and mm_ota does the same. This must match, or they disagree on the live slot.
case "$slot" in
    b*|B*) label=app_b ;;
    *) label=app_a ;;
esac

# Without the label the partition holding the app volume is unknown, and
# attaching a guessed index could mount the wrong slot.
mtd=$(sed -n "s/^mtd\([0-9][0-9]*\): [0-9a-f]* [0-9a-f]* \"${label}\"\$/\1/p" \
    /proc/mtd 2>/dev/null || true)
if [ -z "$mtd" ]; then
    echo "avalon-app-mount: flash partition '${label}' not found;" \
        "the application cannot be mounted" >&2
    exit 1
fi

mkdir -p "$MOUNTPOINT"

cur_mtd=""
[ -r "/sys/class/ubi/ubi${UBIDEV}/mtd_num" ] && cur_mtd=$(cat "/sys/class/ubi/ubi${UBIDEV}/mtd_num")

if [ -n "$cur_mtd" ] && [ "$cur_mtd" != "$mtd" ]; then
    umount "$MOUNTPOINT" 2>/dev/null || true
    ubidetach /dev/ubi_ctrl -d "$UBIDEV"
    cur_mtd=""
fi

if [ -n "$cur_mtd" ] && mountpoint -q "$MOUNTPOINT"; then
    exit 0
fi

if [ -z "$cur_mtd" ]; then
    ubiattach /dev/ubi_ctrl -m "$mtd" -d "$UBIDEV"
fi

mount -t ubifs "ubi${UBIDEV}:${VOL}" "$MOUNTPOINT"
