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

# Run ./build.sh in the pinned Ubuntu 22.04 container; ./build.sh calls it for setup and
# product builds.
#   docker/build.sh --run test/scripts/host_tests.sh   # any command instead of build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$HERE/.." && pwd)"
IMAGE_REPO=canaan-linux-sdk-build
IMAGE_TAG="$(cat "$HERE/Dockerfile" "$HERE/entrypoint.sh" | sha256sum | cut -c1-12)"
IMAGE="${CANAAN_BUILD_IMAGE:-$IMAGE_REPO:$IMAGE_TAG}"

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not found; run ./build.sh setup, which installs it on Ubuntu" >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> building $IMAGE (first run downloads about 3 GB of toolchain)"
    DOCKER_BUILDKIT=1 docker build --label "canaan.worktree=$SDK_ROOT" -t "$IMAGE" "$HERE"
    if [ -z "${CANAAN_BUILD_IMAGE:-}" ]; then
        # docker rmi refuses an image a container still uses.
        docker image ls --filter "label=canaan.worktree=$SDK_ROOT" \
            --format '{{.Repository}}:{{.Tag}}' "$IMAGE_REPO" |
            while read -r old; do
                [ "$old" != "$IMAGE" ] || continue
                if docker rmi "$old" >/dev/null 2>&1; then echo "==> removed $old"; fi
            done
    fi
fi

cmd=(./build.sh "$@")
if [ "${1:-}" = "--run" ]; then
    shift
    [ "$#" -gt 0 ] || { echo "usage: $0 --run COMMAND [ARG...]" >&2; exit 2; }
    cmd=("$@")
fi

tty_flag=()
[ -t 1 ] && tty_flag=(-t)

# Each extra layer's git work tree (else the layer) and the directories of the
# .aup signer and key are mounted at their host paths.
mounts=("$SDK_ROOT")
extra_mount=()
for path in ${CANAAN_EXTRA_LAYERS:-} \
    ${AUP_SIGN_CMD:+"$(dirname "$AUP_SIGN_CMD")"} \
    ${AUP_VERIFY_KEY:+"$(dirname "$AUP_VERIFY_KEY")"}; do
    [ -d "$path" ] || continue
    path="$(git -C "$path" rev-parse --show-toplevel 2>/dev/null || (cd "$path" && pwd))"
    case " ${mounts[*]} " in *" $path "*) continue ;; esac
    mounts+=("$path")
    extra_mount+=(-v "$path":"$path")
done

# Default nofile and pids limits make pseudo fail with "Failed to spawn fakeroot worker".
exec docker run --rm -i "${tty_flag[@]}" \
    --ulimit nofile=1048576:1048576 --ulimit nproc=65535:65535 --pids-limit=-1 \
    -v "$SDK_ROOT":"$SDK_ROOT" "${extra_mount[@]}" -w "$SDK_ROOT" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e SDK_ROOT="$SDK_ROOT" \
    -e CANAAN_IN_CONTAINER=1 \
    -e CANAAN_EXTRA_LAYERS -e AUP_SIGN_CMD -e AUP_VERIFY_KEY -e AUP_VER \
    -e CANAAN_FW_TYPE -e CANAAN_IMAGE_TYPE -e CANAAN_BUILD_REV_SUFFIX -e CANAAN_SDK_VERSION \
    --entrypoint "$HERE/entrypoint.sh" \
    "$IMAGE" "${cmd[@]}"
