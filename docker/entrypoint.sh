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

# Run the command as HOST_UID:HOST_GID (BitBake refuses root), after seeding the
# baked toolchain into $SDK_ROOT/build/yocto.
set -e

uid="${HOST_UID:-1000}"
gid="${HOST_GID:-1000}"
home=/home/builder

if ! getent group "$gid" >/dev/null 2>&1; then
    groupadd -g "$gid" builder
fi
if ! getent passwd "$uid" >/dev/null 2>&1; then
    useradd -u "$uid" -g "$gid" -d "$home" -s /bin/bash builder
fi
mkdir -p "$home"
chown "$uid:$gid" "$home"
export HOME="$home"

# A dir that lacks a bin/ of the image's copy, e.g. a bare .config, is replaced.
seeded() {
    [ ! -L "$2" ] || return 1
    for b in "$1"/bin "$1"/*/bin; do
        [ ! -d "$b" ] || [ -d "$2/${b#"$1"/}" ] || return 1
    done
}

# Copy, not symlink: packaging hard-links toolchain files into build/.
if [ -n "${SDK_ROOT:-}" ]; then
    mkdir -p "$SDK_ROOT/build/yocto"
    for tc in riscv-toolchain-gcc riscv-toolchain-clang riscv-toolchain-gcc-muslc; do
        dst="$SDK_ROOT/build/yocto/$tc"
        [ -d "/opt/canaan/toolchains/$tc" ] || continue
        seeded "/opt/canaan/toolchains/$tc" "$dst" && continue
        rm -rf "$dst"
        tmp="$dst.tmp.$$"
        rm -rf "$tmp"
        cp -a "/opt/canaan/toolchains/$tc" "$tmp"
        chown -R "$uid:$gid" "$tmp"
        mv "$tmp" "$dst"
    done
fi

exec gosu "$uid:$gid" "$@"
