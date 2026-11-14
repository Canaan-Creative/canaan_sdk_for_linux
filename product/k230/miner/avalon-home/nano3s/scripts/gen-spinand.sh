#!/bin/bash
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

# Pack the nano3s SPI-NAND flash image (.kdimg) and signed OTA package (.aup)
# from the Yocto build output into the machine's deploy directory.
#
# Usage: gen-spinand.sh [IMAGE_NAME]    (default: nano3s-image-full)
#        Run by build.sh, which sets CANAAN_BUILD_REV and CANAAN_IMAGE_TYPE.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$SCRIPT_DIR/../../../../../.." && pwd)"

PATH_PREPEND=""
add_path() {
    if [[ -d "$1" ]]; then
        PATH_PREPEND="$1:$PATH_PREPEND"
    fi
}

COMPONENTS="$SDK_ROOT/build/yocto/xuantie-build/xuantie/tmp-glibc/sysroots-components/x86_64"
OE_WORK="$SDK_ROOT/build/yocto/xuantie-build/xuantie/tmp-glibc/work"
add_path "$COMPONENTS/u-boot-tools-native/usr/bin"
add_path "$COMPONENTS/dtc-native/usr/bin"
NATIVE_UBOOT=""
for d in "$OE_WORK/x86_64-linux/u-boot-tools-native"/*/build/tools; do
    if [[ -e "$d" ]]; then
        NATIVE_UBOOT="$d"
        break
    fi
done
add_path "$NATIVE_UBOOT"

MTD_UTILS="$COMPONENTS/mtd-utils-native/usr/sbin"
if [[ -x "$MTD_UTILS/mkfs.ubifs" ]]; then
    add_path "$MTD_UTILS"
fi

[[ -n "$PATH_PREPEND" ]] && export PATH="${PATH_PREPEND}${PATH}"

for tool in mkimage mkenvimage mkfs.ubifs; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: $tool not found — run ./build.sh <product> (it bitbakes" >&2
        echo "       u-boot-tools-native + mtd-utils-native with the image)." >&2
        exit 1
    }
done

# firmware_gen.py needs pycryptodome and gen_aup.py needs cryptography. The
# container image installs both; a venv under build/ reuses them
# (--system-site-packages), so no package index is needed.
PACK_VENV="$SDK_ROOT/build/.pack-venv"
PACK_IMPORTS='import cryptography; from Crypto.Cipher import AES'
mkdir -p "$(dirname "$PACK_VENV")"
exec 9>"$PACK_VENV.lock"
flock 9
if ! "$PACK_VENV/bin/python3" -c "$PACK_IMPORTS" 2>/dev/null; then
    python3 -c "$PACK_IMPORTS" 2>/dev/null || {
        echo "ERROR: python3 has no pycryptodome/cryptography: the container image is" >&2
        echo "       incomplete (docker/Dockerfile installs both)." >&2
        exit 1
    }
    echo "==> bootstrapping pack venv (system pycryptodome + cryptography) at $PACK_VENV"
    python3 -m venv --system-site-packages "$PACK_VENV"
    "$PACK_VENV/bin/python3" -c "$PACK_IMPORTS" || {
        echo "ERROR: pack venv at $PACK_VENV has no pycryptodome/cryptography;" >&2
        echo "       remove it and re-run ./build.sh <product>." >&2
        exit 1
    }
fi
exec 9>&-
export PATH="$PACK_VENV/bin:$PATH"

MACHINE="k230-nano3s"
IMAGE_NAME="${1:-nano3s-image-full}"
DTB="k230-nano3s.dtb"

DEPLOY_DIR="$SDK_ROOT/build/yocto/xuantie-build/xuantie/tmp-glibc/deploy/images/$MACHINE"
GENIMAGE="$COMPONENTS/genimage-native/usr/bin/genimage"
GENIMAGE_CFG="$SCRIPT_DIR/genimage-spinand.cfg"
OUTPUT_DIR="$DEPLOY_DIR"
WORK_DIR="$SDK_ROOT/build/yocto/xuantie-build/xuantie/tmp-glibc/work/pack-$MACHINE"

UBOOT_SPL="$DEPLOY_DIR/fn_u-boot-spl.bin"
UBOOT_FW="$DEPLOY_DIR/fn_ug_u-boot.bin"

TOOLS_DIR="$DEPLOY_DIR/k230-tools"
FW_PAYLOAD="$DEPLOY_DIR/fw_payload.bin"

check_file() {
    [[ -f "$1" ]] || { echo "ERROR: missing: $1"; exit 1; }
}

check_file "$DEPLOY_DIR/Image"
check_file "$DEPLOY_DIR/$DTB"
check_file "$UBOOT_SPL"
check_file "$UBOOT_FW"
check_file "$FW_PAYLOAD"
check_file "$TOOLS_DIR/FW_TEXT_START"
check_file "$TOOLS_DIR/firmware_gen.py"
check_file "$GENIMAGE"
check_file "$GENIMAGE_CFG"

BOARDCONF="$SCRIPT_DIR/../conf/product/boardconf.ini"
GEN_BOARDCONF="$SDK_ROOT/apps/miner/tools/gen_boardconf.py"
check_file "$BOARDCONF"
check_file "$GEN_BOARDCONF"
board_ident="$(python3 "$GEN_BOARDCONF" --sh "$BOARDCONF")" || {
    echo "ERROR: cannot derive HW/SW from $BOARDCONF" >&2
    exit 1
}
eval "$board_ident"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"/{root/linux/uboot,root/rsvd,tmp,output} "$OUTPUT_DIR"
ROOT="$WORK_DIR/root"

echo "==> staging U-Boot SPL + main"
cp "$UBOOT_SPL" "$ROOT/linux/uboot/fn_u-boot-spl.bin"
cp "$UBOOT_FW" "$ROOT/linux/uboot/fn_ug_u-boot.bin"

# The board's compiled-in U-Boot env carries none of HW, SW or the *_partition
# slot variables, so this seed is their only source: without it A/B switching
# never takes effect and every OTA is refused. check_env_seed.py validates the
# generated image.
echo "==> seeding U-Boot environment"
printf '%s\n' \
    'arch=riscv' \
    'autoload=0' \
    'baudrate=115200' \
    'board=k230_nano3s' \
    'board_name=k230_nano3s' \
    'bootdelay=0' \
    'cpu=k230' \
    'vendor=canaan' \
    'loadaddr=0xc000000' \
    'quick_boot=false' \
    "HW=$HW_TYPE" \
    "SW=$SW_TYPE" \
    'uboot_partition=A' \
    'linux_partition=A' \
    'rootfs_partition=A' \
    'app_partition=A' \
    'bootcmd=k230_boot auto auto_boot' \
    > "$WORK_DIR/spinand.env.txt"
mkenvimage -r -s 0x10000 -o "$ROOT/linux/uboot/spinand.env" \
    "$WORK_DIR/spinand.env.txt"
CHECK_ENV_SEED="$SDK_ROOT/apps/miner/tools/check_env_seed.py"
check_file "$CHECK_ENV_SEED"
python3 "$CHECK_ENV_SEED" --product nano3s \
    --env-image "$ROOT/linux/uboot/spinand.env" \
    --aup-config "$SCRIPT_DIR/aup-ota.json"

echo "==> building linux_system.bin"
FW_TEXT_START="$(cat "$TOOLS_DIR/FW_TEXT_START")"
cp "$FW_PAYLOAD" "$WORK_DIR/fw_payload.bin"
gzip -9 -k -f "$WORK_DIR/fw_payload.bin"
# Set the gzip CM byte to 0x09 so U-Boot decompresses with the K230 hardware
# engine; it falls back to software zlib if that fails.
printf '\x09' | dd of="$WORK_DIR/fw_payload.bin.gz" \
    bs=1 seek=2 count=1 conv=notrunc status=none
cp "$DEPLOY_DIR/$DTB" "$WORK_DIR/k230.dtb"
# Placeholder ramdisk, 1 byte.
printf 'a' > "$WORK_DIR/rd"
mkimage -A riscv -O linux -T multi -C gzip \
    -a "$FW_TEXT_START" -e "$FW_TEXT_START" -n linux \
    -d "$WORK_DIR/fw_payload.bin.gz:$WORK_DIR/rd:$WORK_DIR/k230.dtb" \
    "$WORK_DIR/ulinux.bin"
python3 "$TOOLS_DIR/firmware_gen.py" \
    -i "$WORK_DIR/ulinux.bin" \
    -o "$ROOT/linux/linux_system.bin" -n

LS_SIZE=$(stat -c %s "$ROOT/linux/linux_system.bin")
LS_MAX=$((8 * 1024 * 1024))
if [[ "$LS_SIZE" -gt "$LS_MAX" ]]; then
    echo "ERROR: linux_system.bin is $LS_SIZE bytes, exceeds 8 MiB linux_a slot" >&2
    exit 1
fi
echo "    linux_system.bin: $LS_SIZE bytes (load=$FW_TEXT_START)"

# rtt_a/rtt_b are reserved partitions, 0xFF in the .kdimg.
echo "==> staging rtt_system stub"
dd if=/dev/zero bs=1M count=4 status=none | tr '\000' '\377' \
    > "$ROOT/rsvd/rtt_system.bin"

ROOTFS_DIR="${ROOTFS_DIR:-}"
# MACHINE with "-" replaced by "_", as in Yocto work directory names.
MACHINE_UB="${MACHINE//-/_}"
for cand in \
    "$DEPLOY_DIR/${IMAGE_NAME}-${MACHINE}.rootfs" \
    "$OE_WORK/${MACHINE_UB}-oe-linux/${IMAGE_NAME}/1.0-r0/rootfs" \
    "$OE_WORK/${MACHINE}-oe-linux/${IMAGE_NAME}/1.0-r0/rootfs"; do
    if [[ -d "$cand" ]]; then
        ROOTFS_DIR="$cand"
        break
    fi
done
if [[ -z "$ROOTFS_DIR" ]]; then
    echo "ERROR: rootfs tree not found (looked under deploy + work dirs)" >&2
    echo "  set ROOTFS_DIR=/path/to/extracted/rootfs and re-run" >&2
    exit 1
fi
echo "==> rootfs source: $ROOTFS_DIR"

# The app volume holds the contents of /mntapp/release from the app image.
APP_IMAGE_NAME="nano3s-app-image"
APP_IMG_ROOTFS=""
for cand in \
    "$DEPLOY_DIR/${APP_IMAGE_NAME}-${MACHINE}.rootfs" \
    "$OE_WORK/${MACHINE_UB}-oe-linux/${APP_IMAGE_NAME}/1.0-r0/rootfs" \
    "$OE_WORK/${MACHINE}-oe-linux/${APP_IMAGE_NAME}/1.0-r0/rootfs"; do
    if [[ -d "$cand/mntapp/release" ]]; then
        APP_IMG_ROOTFS="$cand"
        break
    fi
done
if [[ -z "$APP_IMG_ROOTFS" ]]; then
    echo "ERROR: ${APP_IMAGE_NAME} tree with mntapp/release not found" >&2
    echo "  build it first: ./build.sh nano3s" >&2
    exit 1
fi
echo "==> app source: $APP_IMG_ROOTFS/mntapp/release"

DATA_SEED="$SCRIPT_DIR/../files"
DATA_ROOT="$ROOT/linux/data"
# The data volume carries the same factory defaults the app volume holds in
# linux/confiles; avalon-data-mount.sh recreates any of them that is missing
# at boot.
mkdir -p "$DATA_ROOT/factory" "$DATA_ROOT/userconfig/wallpaper" "$DATA_ROOT/userdata/log"
cp "$DATA_SEED"/usrcon/* "$DATA_ROOT/userconfig/"
cp "$DATA_SEED"/factory/* "$DATA_ROOT/factory/"
ln -sfn "$ROOTFS_DIR" "$ROOT/linux/rootfs"
ln -sfn "$APP_IMG_ROOTFS/mntapp/release" "$ROOT/linux/app"

echo "==> checking the staged volumes for keys"
leaked="$(find "$ROOTFS_DIR/" "$APP_IMG_ROOTFS/mntapp/release/" "$DATA_ROOT/" \
    \( -name 'ssh_host_*' -o -name '*.tokenkey' -o -name 'miner-web.pem' \
    -o -name '*signing-key*' \) -print)" || {
    echo "ERROR: key scan failed" >&2
    exit 1
}
if [[ -n "$leaked" ]]; then
    echo "ERROR: keys staged for the image:" >&2
    printf '%s\n' "$leaked" >&2
    exit 1
fi

echo "==> running genimage"
LD_LIBRARY_PATH="$COMPONENTS/libconfuse-native/usr/lib:$COMPONENTS/openssl-native/usr/lib" \
    "$GENIMAGE" \
        --rootpath "$ROOT" \
        --inputpath "$ROOT" \
        --outputpath "$WORK_DIR/output" \
        --tmppath "$WORK_DIR/tmp" \
        --config "$GENIMAGE_CFG"

KDIMG="sysimage-spinand256m.kdimg"
[[ -f "$WORK_DIR/output/$KDIMG" ]] || { echo "ERROR: genimage produced no $KDIMG" >&2; exit 1; }

OUTPUT_IMG="$OUTPUT_DIR/${IMAGE_NAME}-${MACHINE}.kdimg"
cp -f "$WORK_DIR/output/$KDIMG" "$OUTPUT_IMG"
( cd "$OUTPUT_DIR" && sha256sum "$(basename "$OUTPUT_IMG")" > "$(basename "$OUTPUT_IMG").sha256" )

# A build that cannot produce the OTA package fails.
AUP_CFG="$SCRIPT_DIR/aup-ota.json"
GEN_AUP="$SDK_ROOT/apps/miner/tools/gen_aup.py"
OUTPUT_AUP="$OUTPUT_DIR/${IMAGE_NAME}-${MACHINE}.aup"
rm -f "$OUTPUT_AUP"

if [[ ! -f "$AUP_CFG" || ! -f "$GEN_AUP" ]]; then
    echo "ERROR: no .aup — need $AUP_CFG and $GEN_AUP" >&2
    exit 1
fi

aup_missing=""
for f in rootfs.ubi app.ubi; do
    if [[ -f "$WORK_DIR/output/$f" ]]; then
        ln -sfn "$WORK_DIR/output/$f" "$ROOT/$f"
    else
        aup_missing="$aup_missing $f"
    fi
done
if [[ -n "$aup_missing" ]]; then
    echo "ERROR: no .aup — genimage produced no$aup_missing" >&2
    exit 1
fi

AUP_VER="${AUP_VER:-01}"
AUP_TYPE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["product"])' \
    "$AUP_CFG" 2>/dev/null || true)"
AUP_SHA="${CANAAN_BUILD_REV:-}"
if [[ -z "$AUP_SHA" ]]; then
    echo "ERROR: no .aup — CANAAN_BUILD_REV is not set; run through build.sh" >&2
    exit 1
fi
AUP_SIGN=()
if [[ -n "${AUP_SIGN_CMD:-}" ]]; then
    AUP_SIGN=(--sign-cmd "$AUP_SIGN_CMD" --verify-key "${AUP_VERIFY_KEY:-}")
fi
echo "==> building OTA .aup"
rm -rf "$WORK_DIR/aup"
mkdir -p "$WORK_DIR/aup"
AUP_JSON="$WORK_DIR/aup-ota.json"
sed -e "s/CONFIG_IMG_HW/$HW_TYPE/g" -e "s/CONFIG_IMG_SW/$SW_TYPE/g" \
    "$AUP_CFG" > "$AUP_JSON"
if grep -q 'CONFIG_IMG_' "$AUP_JSON"; then
    echo "ERROR: no .aup — $AUP_CFG has CONFIG_IMG_* left after" >&2
    echo "       expansion from $BOARDCONF" >&2
    exit 1
fi
if [[ -z "$AUP_TYPE" ]]; then
    echo "ERROR: no .aup — $AUP_CFG declares no \"product\"" >&2
    exit 1
fi
if ! python3 "$GEN_AUP" -f "$AUP_JSON" -i "$ROOT" -o "$WORK_DIR/aup" \
        -t "$AUP_TYPE" -r "$CANAAN_IMAGE_TYPE" -v "$AUP_VER" -m "$AUP_SHA" \
        --firmware "$APP_IMG_ROOTFS/mntapp/release/linux/app/mm_miner" "${AUP_SIGN[@]}"; then
    echo "ERROR: no .aup — $GEN_AUP failed" >&2
    exit 1
fi
aup_src=""
for f in "$WORK_DIR/aup"/*.aup; do
    if [[ -e "$f" ]]; then
        aup_src="$f"
        break
    fi
done
if [[ -z "$aup_src" ]]; then
    echo "ERROR: no .aup — $GEN_AUP wrote no file" >&2
    exit 1
fi
mv -f "$aup_src" "$OUTPUT_AUP"

rm -rf "$WORK_DIR"

echo ""
echo "==> done: $OUTPUT_IMG"
echo "          $OUTPUT_IMG.sha256"
echo "    size: $(du -sh "$OUTPUT_IMG" | cut -f1)"
if [[ -f "$OUTPUT_AUP" ]]; then
    echo "          $OUTPUT_AUP"
    echo "    aup : $(stat -c %s "$OUTPUT_AUP") bytes"
fi
