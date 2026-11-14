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

# Attach the "data" MTD partition as ubi2 and mount its UBIFS volume at /data,
# then create every /data file and directory the miner needs that is missing,
# from the factory defaults on the app volume in /mntapp/release/linux/confiles;
# existing files are kept. The app volume must be mounted first; when it is
# not, nothing is created and the script warns.
# If the partition cannot be mounted, the files are created in /data on the root
# file system instead and the script exits non-zero.
# The MTD number is looked up by partition label in /proc/mtd, never hardcoded.
# Idempotent; run by the avalon-miner init script before the miner starts.
set -e

MOUNTPOINT=/data
UBIDEV=2
VOL=ubi_data_part
LABEL=data
APPMOUNT=/mntapp/release
DEFAULTS=$APPMOUNT/linux/confiles

seed() {
    rc=0
    for f in "$1"/*; do
        [ -f "$f" ] || continue
        dst="$2/${f##*/}"
        [ -e "$dst" ] && continue
        if cp "$f" "$dst.tmp" && mv "$dst.tmp" "$dst"; then
            echo "avalon-data-mount: ${f##*/} missing; created from the factory default"
        else
            rc=1
        fi
    done
    return $rc
}

mounted=1
mtd=$(sed -n "s/^mtd\([0-9][0-9]*\): [0-9a-f]* [0-9a-f]* \"${LABEL}\"\$/\1/p" \
    /proc/mtd 2>/dev/null || true)
if [ -z "$mtd" ]; then
    echo "avalon-data-mount: flash partition '${LABEL}' not found" >&2
    mounted=0
fi

mkdir -p "$MOUNTPOINT"

cur_mtd=""
[ -r "/sys/class/ubi/ubi${UBIDEV}/mtd_num" ] && cur_mtd=$(cat "/sys/class/ubi/ubi${UBIDEV}/mtd_num")

if [ "$mounted" = 1 ] && { [ -z "$cur_mtd" ] || ! mountpoint -q "$MOUNTPOINT"; }; then
    if [ -z "$cur_mtd" ]; then
        ubiattach /dev/ubi_ctrl -m "$mtd" -d "$UBIDEV" || mounted=0
    fi
    if [ "$mounted" = 1 ]; then
        mount -t ubifs "ubi${UBIDEV}:${VOL}" "$MOUNTPOINT" || mounted=0
    fi
fi
if [ "$mounted" != 1 ]; then
    echo "avalon-data-mount: data partition not mounted; using default settings on the root" \
        "file system; changes are not saved to the data partition" >&2
fi

ok=1
mkdir -p "$MOUNTPOINT/userconfig/wallpaper" "$MOUNTPOINT/factory" "$MOUNTPOINT/userdata/log" || ok=0
if mountpoint -q "$APPMOUNT"; then
    seed "$DEFAULTS/usrcon" "$MOUNTPOINT/userconfig" || ok=0
    seed "$DEFAULTS/factory" "$MOUNTPOINT/factory" || ok=0
else
    echo "avalon-data-mount: application partition not mounted; missing settings files" \
        "were not created from the factory defaults" >&2
fi
sync
if [ "$ok" != 1 ]; then
    echo "avalon-data-mount: some missing settings files could not be created" >&2
fi
[ "$mounted" = 1 ]
