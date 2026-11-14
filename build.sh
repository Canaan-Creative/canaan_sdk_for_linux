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

# Canaan SDK build script; ./build.sh --help lists the commands.
#
# setup and a product build re-run themselves in the pinned Ubuntu 22.04 container
# through docker/build.sh.

set -euo pipefail

SDK_ROOT="$(cd "$(dirname "$0")" && pwd)"
YOCTO_ROOT="$SDK_ROOT/build/yocto"
BUILD_DIR="$YOCTO_ROOT/xuantie-build/xuantie"
OE_INIT="$YOCTO_ROOT/openembedded-core/oe-init-build-env"
LOCAL_CONF="$BUILD_DIR/conf/local.conf"

ORIG_ARGS=("$@")
DOCKER_BUILD="$SDK_ROOT/docker/build.sh"

CANAAN_FW_TYPE="${CANAAN_FW_TYPE:-Debug}"
CANAAN_IMAGE_TYPE="${CANAAN_IMAGE_TYPE:-dbg}"
SDK_VERSION="${CANAAN_SDK_VERSION:-$(tr -d '[:space:]' < "$SDK_ROOT/VERSION")}"
MINER_VERSION="$(tr -d '[:space:]' < "$SDK_ROOT/apps/miner/VERSION" 2>/dev/null || true)"
[[ -n "${MINER_VERSION:-}" ]] || MINER_VERSION="$SDK_VERSION"

# One xuantie-yocto release archive (tag XuanTie_V1.3.2-6.6, commit
# 0ff6ba270ee1838da932d20c7e20352d15e984a0) carries every metadata layer.
XUANTIE_YOCTO_VERSION="1.3.2"
XUANTIE_YOCTO_COMMIT="0ff6ba270ee1838da932d20c7e20352d15e984a0"
XUANTIE_YOCTO_DIR="xuantie-yocto-${XUANTIE_YOCTO_COMMIT}"
XUANTIE_YOCTO_ARCHIVE="${XUANTIE_YOCTO_DIR}.tar.gz"
XUANTIE_YOCTO_URL="https://github.com/XUANTIE-RV/xuantie-yocto/archive/"
XUANTIE_YOCTO_URL+="${XUANTIE_YOCTO_COMMIT}.tar.gz"
XUANTIE_YOCTO_SHA256="7b63e9eb8f51f25462c2e3df7e338306b0099e8b31b0b1045a73d79b8989f71b"
XUANTIE_STAMP="$YOCTO_ROOT/.xuantie-version"

XUANTIE_META_LAYERS=(
    openembedded-core
    meta-xuantie
    meta-k230
    meta-openembedded
    meta-riscv
    meta-clang
    meta-external-toolchain
)
fetch_verified() {
    local url="$1" path="$2" sha256="$3"
    if [[ -f "$path" ]]; then
        echo "  use cached: $path"
    else
        echo "  download: $url"
        curl -fL --retry 3 -o "$path.part" "$url"
        mv -f "$path.part" "$path"
    fi
    if ! printf '%s  %s\n' "$sha256" "$path" | sha256sum -c --status -; then
        echo "error: $path does not have sha256 $sha256; remove it and re-run setup" >&2
        exit 1
    fi
}

apply_patches() {
    local layer="$1" patch
    shift
    for patch in "$@"; do
        [[ -f "$patch" ]] || continue
        echo "  patch $layer: $(basename "$patch")"
        if ! patch -d "$YOCTO_ROOT/$layer" -p1 --forward --silent -r /dev/null < "$patch"; then
            echo "error: $patch does not apply to $YOCTO_ROOT/$layer" >&2
            exit 1
        fi
    done
}

cmd_setup() {
    echo "==> Updating submodules (u-boot, opensbi, linux) ..."
    # sync first: an already-inited submodule keeps its old URL otherwise.
    git -C "$SDK_ROOT" submodule sync --recursive
    git -C "$SDK_ROOT" submodule update --init --recursive --recommend-shallow

    echo ""
    echo "==> Fetching Yocto layers ($XUANTIE_YOCTO_VERSION) from GitHub ..."
    mkdir -p "$YOCTO_ROOT"

    # The layers are replaced from the archive and patched whenever the stamp
    # (archive version and a hash of the local patches) differs.
    PATCHES_DIR="$YOCTO_ROOT/patches"
    layers_id="$XUANTIE_YOCTO_VERSION $(cat "$PATCHES_DIR"/*.patch 2>/dev/null |
        sha256sum | cut -c1-16)"
    if [[ "$(cat "$XUANTIE_STAMP" 2>/dev/null || true)" == "$layers_id" ]]; then
        echo "  layers already at $XUANTIE_YOCTO_VERSION with the local patches (skip fetch)"
    else
        XUANTIE_SRC="$SDK_ROOT/.xuantie-yocto-src"
        ARCHIVE_PATH="$XUANTIE_SRC/$XUANTIE_YOCTO_ARCHIVE"
        SRC_TOP="$XUANTIE_SRC/$XUANTIE_YOCTO_DIR"
        mkdir -p "$XUANTIE_SRC"
        fetch_verified "$XUANTIE_YOCTO_URL" "$ARCHIVE_PATH" "$XUANTIE_YOCTO_SHA256"
        if [[ ! -d "$SRC_TOP" ]]; then
            echo "  unpack: $XUANTIE_YOCTO_ARCHIVE"
            tar -xzf "$ARCHIVE_PATH" -C "$XUANTIE_SRC"
        fi
        for layer in "${XUANTIE_META_LAYERS[@]}"; do
            echo "  refresh: $layer"
            rm -rf "${YOCTO_ROOT:?}/$layer"
            cp -a "$SRC_TOP/$layer" "$YOCTO_ROOT/$layer"
        done

        echo ""
        echo "==> Applying local patches to upstream layers ..."
        apply_patches openembedded-core "$PATCHES_DIR"/0001-bitbake-*.patch
        apply_patches meta-xuantie "$PATCHES_DIR"/*-meta-xuantie-*.patch
        apply_patches meta-external-toolchain "$PATCHES_DIR"/0003-meta-external-toolchain-*.patch
        echo "$layers_id" > "$XUANTIE_STAMP"
    fi

    # docker/entrypoint.sh copies the toolchains baked into the image.
    if [[ -d "$YOCTO_ROOT/riscv-toolchain-gcc/bin" &&
        -d "$YOCTO_ROOT/riscv-toolchain-clang/bin" ]]; then
        echo "  toolchains ready (copied from the container image)"
    else
        echo "error: the XuanTie toolchains are missing from build/yocto; re-run" \
            "./build.sh setup, whose container copies them from the image" >&2
        exit 1
    fi

    echo ""
    echo "==> Setup complete. Run './build.sh' to list products."
}

run_in_docker() {
    if ! docker info >/dev/null 2>&1 &&
        { ! command -v docker >/dev/null 2>&1 || ! id -nG "$(id -un)" | grep -qw docker; }; then
        if grep -qx 'ID=ubuntu' /etc/os-release 2>/dev/null; then
            "$SDK_ROOT/docker/install-docker.sh"
        else
            echo "error: Docker is not usable here; install it and add your user to the docker" \
                "group" >&2
            exit 1
        fi
    fi

    if docker info >/dev/null 2>&1; then
        exec "$DOCKER_BUILD" "$@"
    fi
    # A docker group added in this login session is active only after the next login.
    exec sg docker -c "$(printf '%q ' "$DOCKER_BUILD" "$@")"
}

run_clean() {
    local dry="$1" assume_yes="$2" prompt="$3"; shift 3
    local paths=("$@") ans=""

    # -X removes only git-ignored files; -ff also removes nested git repos.
    echo "==> Generated content to remove (git-ignored only):"
    git -C "$SDK_ROOT" clean -dXff -n -- "${paths[@]}" | sed 's/^Would remove /  /'

    if [[ "$dry" -eq 1 ]]; then
        echo "(dry-run) nothing removed."
        return 0
    fi
    if [[ "$assume_yes" -ne 1 ]]; then
        read -r -p "$prompt [y/N] " ans || true
        [[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted."; return 0; }
    fi
    git -C "$SDK_ROOT" clean -dXff -- "${paths[@]}"
}

parse_clean_flags() {
    CLEAN_DRY=0; CLEAN_YES=0
    local a
    for a in "$@"; do
        case "$a" in
            -y|--yes) CLEAN_YES=1 ;;
            -n|--dry-run) CLEAN_DRY=1 ;;
            *) echo "unknown option '$a'" >&2; exit 1 ;;
        esac
    done
}

cmd_clean() {
    parse_clean_flags "$@"
    run_clean "$CLEAN_DRY" "$CLEAN_YES" \
        "Remove bitbake build output (layers kept)?" \
        build/yocto/xuantie-build
    [[ "$CLEAN_DRY" -eq 1 ]] && return 0
    echo ""
    echo "==> Build output cleaned. Layers kept — just rebuild (no setup needed):"
    echo "    ./build.sh <product>"
}

cmd_cleanall() {
    parse_clean_flags "$@"
    local paths=("$XUANTIE_STAMP") layer
    local prompt="Remove the Yocto layers fetched by setup"
    prompt+=" (archive, downloads, sstate and toolchains kept)?"
    for layer in "${XUANTIE_META_LAYERS[@]}"; do
        paths+=("$YOCTO_ROOT/$layer")
    done

    run_clean "$CLEAN_DRY" "$CLEAN_YES" "$prompt" "${paths[@]}"
    [[ "$CLEAN_DRY" -eq 1 ]] && return 0
    echo ""
    echo "==> Layers removed. Restore them from the cached archive before building:"
    echo "    ./build.sh setup"
}

find_product_conf() {
    find "$SDK_ROOT/product" -name "build.conf" -path "*/$1/build.conf" | head -1
}

list_products() {
    echo "Available products:"
    while IFS= read -r conf; do
        local name MACHINE DISTRO IMAGE
        name="$(basename "$(dirname "$conf")")"
        MACHINE="" DISTRO="" IMAGE=""
        # shellcheck source=/dev/null
        . "$conf"
        printf "  %-24s  MACHINE=%-20s  DISTRO=%-18s  IMAGE=%s\n" \
            "$name" "$MACHINE" "$DISTRO" "$IMAGE"
    done < <(find "$SDK_ROOT/product" -name "build.conf" | sort)
}

list_options() {
    echo "Miner firmware options:"
    printf "  %-24s  %s\n" "-v=, --version=NN" \
        "two decimal digits, the build revision in the"
    printf "  %-24s  %s\n" "" \
        "firmware version string; defaults to 01"
}

usage() {
    cat <<'EOF'
Usage:
  ./build.sh setup                            fetch all source (submodules + Yocto layers)
  ./build.sh clean [-n] [-y]                  remove bitbake build output, downloads and sstate
  ./build.sh cleanall [-n] [-y]               remove the Yocto layers so setup restores them
  ./build.sh                                  list available products
  ./build.sh <product> [options] [image] [bitbake args]

setup and <product> run in the pinned Ubuntu 22.04 container; on Ubuntu they first install
Docker when it is missing or you are not in the docker group.

Build options:
  --rebuild-bsp             cleansstate u-boot + opensbi first (off by default)
  -h, --help                this text

EOF
    list_options
    cat <<'EOF'

Examples:
  ./build.sh nano3s                           the product's default image
  ./build.sh nano3s canaan-image-minimal      a specific image
  ./build.sh nano3s -c compile u-boot         extra bitbake args
  ./build.sh nano3s -v=07                     miner version number
EOF
}

if [[ $# -eq 0 ]]; then
    list_products
    echo ""
    list_options
    exit 0
fi

case "$1" in
    -h|--help) usage; exit 0 ;;
    setup) [[ -n "${CANAAN_IN_CONTAINER:-}" ]] || run_in_docker setup; cmd_setup; exit 0 ;;
    clean) shift; cmd_clean "$@"; exit 0 ;;
    cleanall) shift; cmd_cleanall "$@"; exit 0 ;;
esac

PRODUCT="$1"; shift

REBUILD_BSP=0
MINER_BUILD_REV=""
REMAINING_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --rebuild-bsp) REBUILD_BSP=1 ;;
        --native)
            echo "error: --native is no longer supported; builds run in the container" >&2
            exit 2 ;;
        -v=*|--version=*) MINER_BUILD_REV="${arg#*=}" ;;
        -h|--help) usage; exit 0 ;;
        *) REMAINING_ARGS+=("$arg") ;;
    esac
done
set -- "${REMAINING_ARGS[@]+"${REMAINING_ARGS[@]}"}"

if [[ ! "$SDK_VERSION" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then
    echo "error: CANAAN_SDK_VERSION '$SDK_VERSION' is not dot-separated decimal numbers" >&2
    exit 1
fi

if [[ -n "$MINER_BUILD_REV" && ! "$MINER_BUILD_REV" =~ ^[0-9][0-9]$ ]]; then
    echo "error: version number '$MINER_BUILD_REV' is not two decimal digits" >&2
    echo "usage: ./build.sh $PRODUCT -v=NN   (e.g. -v=07; ./build.sh --help)" >&2
    exit 1
fi
MINER_BUILD_PASS=""
if [[ -n "$MINER_BUILD_REV" ]]; then
    export MINER_BUILD_REV
    MINER_BUILD_PASS="$MINER_BUILD_PASS MINER_BUILD_REV"
fi

PRODUCT_CONF="$(find_product_conf "$PRODUCT")"
if [[ -z "$PRODUCT_CONF" ]]; then
    echo "error: product '$PRODUCT' not found" >&2
    echo ""
    list_products
    exit 1
fi

MACHINE="" DISTRO="" IMAGE=""
# shellcheck source=/dev/null
. "$PRODUCT_CONF"

if [[ $# -gt 0 && "$1" != -* ]]; then
    IMAGE="$1"; shift
fi

EXTRA_ARGS=("$@")

# -n/-c/-b build no image: no pack step and nothing to collect.
TASK_ONLY=0
case " ${EXTRA_ARGS[*]:-} " in
    *" -n "*|*" --dry-run "*|*" -c "*|*" --cmd "*|*" -b "*) TASK_ONLY=1; PACK_SCRIPT="" ;;
esac

for var in MACHINE DISTRO IMAGE; do
    if [[ -z "${!var}" ]]; then
        echo "error: $var not set in $PRODUCT_CONF" >&2
        exit 1
    fi
done

if [[ ! -f "$LOCAL_CONF" ]]; then
    echo "error: local.conf not found at $LOCAL_CONF" >&2
    exit 1
fi

if [[ ! -f "$OE_INIT" ]]; then
    echo "error: oe-init-build-env not found at $OE_INIT;" \
        "fetch the Yocto layers with ./build.sh setup first" >&2
    exit 1
fi

# Native tools bake absolute paths into tmp-glibc, so a tree built at another
# path is wiped and regenerated from sstate.
TMP_GLIBC="$BUILD_DIR/tmp-glibc"
if [[ -f "$TMP_GLIBC/saved_tmpdir" ]]; then
    saved="$(cat "$TMP_GLIBC/saved_tmpdir")"
    if [[ "$saved" != "$TMP_GLIBC" ]]; then
        echo "WARNING: $TMP_GLIBC was built at a different path:" >&2
        echo "         saved: $saved" >&2
        echo "         here : $TMP_GLIBC" >&2
        echo "         Copied/moved TMPDIRs are not relocatable — wiping tmp-glibc" >&2
        echo "         (+ bitbake cache); it regenerates from sstate." >&2
        rm -rf "$TMP_GLIBC" "$BUILD_DIR/cache"
    fi
fi

DISTRO_VERSION="$SDK_VERSION"

# rv64-k6.6-default.conf hard-sets MACHINE; canaan-k230.conf re-asserts it from
# CANAAN_MACHINE.
CANAAN_MACHINE="$MACHINE"
export CANAAN_MACHINE MACHINE DISTRO
CANAAN_SDK_VERSION="$SDK_VERSION"
CANAAN_MINER_VERSION="$MINER_VERSION"
export CANAAN_SDK_VERSION CANAAN_MINER_VERSION CANAAN_FW_TYPE CANAAN_IMAGE_TYPE

# Eight characters: the first 8 hex digits of HEAD, or the first 7 and "t"
# when a tracked file other than a submodule is modified.
git_rev() {
    local sha
    sha="$(git -C "$1" rev-parse --short=8 HEAD 2>/dev/null || echo nogit)"
    if [[ "$sha" != "nogit" &&
        -n "$(git -C "$1" status --porcelain --untracked-files=no \
            --ignore-submodules=all 2>/dev/null)" ]]; then
        sha="${sha:0:7}t"
    fi
    printf '%s\n' "${sha:0:8}"
}
CANAAN_GIT_REV="$(git_rev "$SDK_ROOT")"
export CANAAN_GIT_REV
if [[ -n "${CANAAN_BUILD_REV_SUFFIX:-}" && ! "$CANAAN_BUILD_REV_SUFFIX" =~ ^[0-9A-Za-z]+$ ]]; then
    echo "error: CANAAN_BUILD_REV_SUFFIX '$CANAAN_BUILD_REV_SUFFIX' is not letters and digits" >&2
    exit 1
fi
CANAAN_BUILD_REV="${CANAAN_GIT_REV}${CANAAN_BUILD_REV_SUFFIX:+_$CANAAN_BUILD_REV_SUFFIX}"
export CANAAN_BUILD_REV

_date="$(date +%Y%m%d_%H%M)"
MINER_BUILD_TIME="${_date/_/}"
export MINER_BUILD_TIME
MINER_BUILD_PASS="$MINER_BUILD_PASS MINER_BUILD_TIME"

[[ -n "${AUP_VER:-}" ]] && export AUP_VER

echo "product : $PRODUCT"
echo "version : $DISTRO_VERSION"
echo "machine : $MACHINE"
echo "distro  : $DISTRO"
echo "image   : $IMAGE"
[[ -n "${CANAAN_EXTRA_LAYERS:-}" ]] && echo "layers  : $CANAAN_EXTRA_LAYERS"
echo "fw type : $CANAAN_FW_TYPE ($CANAAN_IMAGE_TYPE)"
[[ -n "$MINER_BUILD_REV" ]] && echo "fw rev  : $MINER_BUILD_REV"
[[ ${#EXTRA_ARGS[@]} -gt 0 ]] && echo "extra   : ${EXTRA_ARGS[*]}"
echo ""

if [[ -z "${CANAAN_IN_CONTAINER:-}" ]]; then
    echo "==> building in the pinned Ubuntu 22.04 container" >&2
    run_in_docker "${ORIG_ARGS[@]}"
fi

(
    set +u
    # shellcheck source=/dev/null
    source "$OE_INIT" "$BUILD_DIR" > /dev/null
    set -u
    # BitBake filters the environment before it parses local.conf, so the
    # passthrough list must be set here.
    BB_ENV_PASSTHROUGH_ADDITIONS="${BB_ENV_PASSTHROUGH_ADDITIONS:-} CANAAN_MACHINE"
    BB_ENV_PASSTHROUGH_ADDITIONS+=" CANAAN_SDK_VERSION CANAAN_MINER_VERSION CANAAN_GIT_REV"
    BB_ENV_PASSTHROUGH_ADDITIONS+=" CANAAN_FW_TYPE CANAAN_IMAGE_TYPE CANAAN_EXTRA_LAYERS"
    BB_ENV_PASSTHROUGH_ADDITIONS+="${MINER_BUILD_PASS}"
    export BB_ENV_PASSTHROUGH_ADDITIONS
    if [[ "$REBUILD_BSP" -eq 1 ]]; then
        echo "==> --rebuild-bsp: cleansstate u-boot opensbi"
        bitbake -c cleansstate u-boot opensbi
    fi
    # The pack step runs these native tools from tmp-glibc/sysroots-components.
    PACK_NATIVE_DEPS=()
    if [[ -n "${PACK_SCRIPT:-}" ]]; then
        PACK_NATIVE_DEPS=(u-boot-tools-native mtd-utils-native genimage-native)
    fi
    read -ra extra_images <<< "${EXTRA_IMAGES:-}"
    bitbake "$IMAGE" "${extra_images[@]}" "${PACK_NATIVE_DEPS[@]}" "${EXTRA_ARGS[@]}"
)

# PACK_SCRIPT (build.conf, relative to the product dir) makes the flashable image.
if [[ -n "${PACK_SCRIPT:-}" ]]; then
    PRODUCT_DIR="$(dirname "$PRODUCT_CONF")"
    PACK_PATH="$PRODUCT_DIR/$PACK_SCRIPT"
    if [[ -x "$PACK_PATH" ]]; then
        echo ""
        echo "==> running pack: $PACK_SCRIPT"
        "$PACK_PATH" "$IMAGE"
    else
        echo "warning: PACK_SCRIPT '$PACK_SCRIPT' not executable at $PACK_PATH" >&2
    fi
fi

# Naming: docs/BUILD.md "Output images".
#   <Line>_<model>[_<hw>_<sw>]_s<SDK>_m<MINER>_<bld>_<YYYYMMDD>_<HHMM>_<rev>[_<suffix>]

boardconf_key() {
    grep -E "^[[:space:]]*$2[[:space:]]*=" "$1" | head -1 \
        | sed -e 's/;.*//' -e 's/.*=[[:space:]]*//' -e 's/"//g' \
        | tr -d '[:space:]_' | tr '[:upper:]' '[:lower:]' || true
}

img_line() {
    if [[ -e "$1" ]]; then
        printf "   %-52s %7s\n" "$(basename "$1")" "$(du -hL "$1" 2>/dev/null | cut -f1)"
    fi
    return 0
}

if [[ "$TASK_ONLY" -eq 0 ]]; then
    DEPLOY_IMG="$BUILD_DIR/tmp-glibc/deploy/images/$MACHINE"
    OUT_DIR="$SDK_ROOT/build/images"

    _model="$(printf '%s' "$PRODUCT" | tr '[:upper:]' '[:lower:]')"
    # product/<chip>/miner/<line>/<model>/build.conf
    _line="${PRODUCT_CONF##*/miner/}"; _line="${_line%%/*}"
    case "$_line" in
        avalon-home) _prefix="AvalonHome" ;;
        *) _prefix="Avalon" ;;
    esac
    _ident="${_prefix}_${_model}"
    _bc="$(dirname "$PRODUCT_CONF")/conf/product/boardconf.ini"
    if [[ -f "$_bc" ]]; then
        _hw="$(boardconf_key "$_bc" HW_TYPE)"
        _sw="$(boardconf_key "$_bc" SW_TYPE)"
        [[ -z "$_hw" ]] || _ident="${_ident}_${_hw}"
        [[ -z "$_sw" ]] || _ident="${_ident}_${_sw}"
    fi
    BASENAME="${_ident}_s${SDK_VERSION}_m${MINER_VERSION}_${CANAAN_IMAGE_TYPE}"
    BASENAME+="_${_date}_${CANAAN_BUILD_REV}"

    echo ""
    echo "============================================================"
    echo " Built images — $PRODUCT"
    echo "   MACHINE=$MACHINE  DISTRO=$DISTRO  IMAGE=$IMAGE"
    echo "   name: $BASENAME"
    echo "============================================================"

    if [[ -d "$DEPLOY_IMG" ]]; then
        mkdir -p "$OUT_DIR"
        src="$DEPLOY_IMG/$IMAGE-$MACHINE"
        if [[ -e "$src.kdimg" ]]; then
            cp -f "$src.kdimg" "$OUT_DIR/${BASENAME}.kdimg"
            # gzip after the rename, so the archive records the versioned name.
            gzip -9 -f "$OUT_DIR/${BASENAME}.kdimg"
        fi
        if [[ -e "$src.aup" ]]; then
            # A hard link, not a symlink: the next build rewrites the deploy
            # name. Read-only, so an in-place write through either name fails
            # instead of changing a published package.
            cp -lf "$src.aup" "$OUT_DIR/${BASENAME}.aup" 2>/dev/null \
                || cp -f "$src.aup" "$OUT_DIR/${BASENAME}.aup"
            chmod a-w "$OUT_DIR/${BASENAME}.aup"
        fi

        echo " output images (build/images/):"
        shopt -s nullglob
        any=0
        for f in "$OUT_DIR/$BASENAME".*; do
            case "$f" in *.sha256) continue;; esac
            img_line "$f"; any=1
        done
        shopt -u nullglob
        [[ "$any" -eq 1 ]] || echo "   (no flashable image produced — built rootfs only)"

        primary=""
        if [[ -e "$OUT_DIR/$BASENAME.kdimg.gz" ]]; then
            primary="$OUT_DIR/$BASENAME.kdimg.gz"
        fi
        if [[ -n "${primary:-}" && -e "$primary" ]]; then
            ( cd "$OUT_DIR" &&
                sha256sum "$(basename "$primary")" > "$(basename "$primary").sha256" )
            echo "   sha256: $(cut -d' ' -f1 "$primary.sha256")  ($(basename "$primary").sha256)"
        fi
        if [[ -e "$OUT_DIR/${BASENAME}.kdimg.gz" ]]; then
            echo "   flash : gunzip -k build/images/${BASENAME}.kdimg.gz &&" \
                "kburn -m nand build/images/${BASENAME}.kdimg"
        fi
        if [[ -e "$OUT_DIR/${BASENAME}.aup" ]]; then
            echo "   ota   : ${BASENAME}.aup"
        fi

        echo " boot artifacts (deploy):  $DEPLOY_IMG"
        for f in "$DEPLOY_IMG"/Image "$DEPLOY_IMG/$MACHINE".dtb "$DEPLOY_IMG"/fw_payload.bin \
            "$DEPLOY_IMG"/fn_u-boot-spl.bin "$DEPLOY_IMG"/fn_ug_u-boot.bin; do
            img_line "$f"
        done
    else
        echo "   (deploy dir not found — did bitbake produce an image?)"
    fi
    echo ""
fi
