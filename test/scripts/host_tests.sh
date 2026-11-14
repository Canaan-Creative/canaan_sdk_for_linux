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

# Host gate (hardware-free), the single entry point for this repository's own
# tests. Runs conf_check.sh and the shellcheck.sh lint
# first, one assertion each, then configures, builds and runs every host ctest
# suite — common (lay1), osif (lay2), hal (lay4),
# miner_core + miner_mgmt (lay3), miner-web, the behavior harness and
# sha256-hwcmp — plus the build-time Python tools under
# apps/miner/tools, which are not CMake trees and so get their own runner below.
# It also builds mc_mgmt and core_miner to check the embedded public keys, the
# libcrypto link and the build with the API plugin stub.
# One assertion per suite,
# green only if that suite's ctest passes, plus one TAP "# SKIP" line per
# ctest case that skipped. JUnit at test/results/host-tests.xml,
# per-suite logs at test/results/logs/host-<suite>.log.
# Each build and each ctest run uses HOST_TESTS_JOBS parallel jobs (default:
# nproc); HOST_TESTS_JOBS=1 builds and runs serially.
#
# Build directories stay inside the source tree so that work trees sharing /tmp
# do not share a CMake cache, which aborts the configure on a source-dir mismatch.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$HERE/../.." && pwd)"
source "$HERE/lib/assert.sh"
tap_init "host-tests" "$SDK_ROOT/test/results/host-tests.xml"

MINER="$SDK_ROOT/apps/miner"

HOST_TESTS_JOBS="${HOST_TESTS_JOBS:-$(nproc)}"
if ! [[ "$HOST_TESTS_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "HOST_TESTS_JOBS must be a positive integer, got '$HOST_TESTS_JOBS'" >&2
    exit 2
fi
export HOST_TESTS_JOBS

# OpenSSL for the RSA-verify suites (miner_mgmt aup_verify and core_sigverify,
# miner_core wallpaper, and every core_miner build) and the miner-web HTTPS
# certificate. A build host may carry
# no system libssl-dev; OPENSSL_ROOT_DIR, else the Yocto openssl-native sysroot
# staged under build/, is then the host openssl. Locate it and hand it to those
# suites; if absent, fall back to whatever FindOpenSSL discovers on the system.
OSSL_ARGS=()
_ossl="${OPENSSL_ROOT_DIR:-}"
if [ -z "$_ossl" ]; then
    for _d in \
        "$SDK_ROOT"/build/yocto/*/*/tmp-glibc/sysroots-components/x86_64/openssl-native/usr; do
        if [ -e "$_d" ]; then
            _ossl="$_d"
            break
        fi
    done
fi
if [ -n "$_ossl" ] && [ -f "$_ossl/include/openssl/opensslv.h" ] &&
    [ -e "$_ossl/lib/libcrypto.so" ]; then
    OSSL_ARGS=(-DOPENSSL_ROOT_DIR="$_ossl"
        -DOPENSSL_CRYPTO_LIBRARY="$_ossl/lib/libcrypto.so"
        -DOPENSSL_INCLUDE_DIR="$_ossl/include")
fi

# miner_mgmt downloads LVGL; LVGL_SOURCE_DIR names a local checkout of the same
# commit instead.
LVGL_ARGS=()
[ -z "${LVGL_SOURCE_DIR:-}" ] || LVGL_ARGS=(-DFETCHCONTENT_SOURCE_DIR_LVGL="$LVGL_SOURCE_DIR")

# Per-suite configure/build/ctest logs, kept INSIDE the repo under
# test/results/logs/ (gitignored, next to the JUnit result) —
# never /tmp. A /tmp log is separated from the result it explains, and a /tmp *build*
# dir is worse: it is shared across worktrees, so a stale CMake cache from
# another tree makes cmake abort on a source-dir mismatch and the whole result
# is bogus. Build dirs stay in-tree ($src/build) for the same reason.
LOGDIR="$SDK_ROOT/test/results/logs"
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/host-*.log

# suite <name> <cmake-source-dir> [extra cmake args...]
suite() {
    local name="$1" src="$MINER/$2"; shift 2
    local bdir="$src/build"
    # clean build: a reused workspace keeps a stale CMake cache that misses
    # source-list changes -> undefined refs
    rm -rf "$bdir"
    local log="$LOGDIR/host-$name.log"
    if cmake -S "$src" -B "$bdir" "$@" >"$log" 2>&1 &&
        cmake --build "$bdir" --parallel "$HOST_TESTS_JOBS" >>"$log" 2>&1 &&
        ctest --test-dir "$bdir" -j "$HOST_TESTS_JOBS" --output-on-failure >>"$log" 2>&1; then
        assert_pass "$name host ctest"
        # ctest counts a case that exits with its SKIP_RETURN_CODE as passed;
        # report each one as a TAP skip so the summary shows what did not run.
        local t
        while read -r t; do
            assert_skip "$name $t" "ctest case skipped"
        done < <(sed -n 's/^.*Test  *#[0-9]*: \([^ ]*\) .*\*\*\*Skipped.*$/\1/p' "$log")
    else
        # assert_fail's detail reaches the JUnit XML only, so the console says
        # "not ok" and nothing else -- and the XML is the only copy of why.
        # Echo the reason as TAP comments too (legal, ignored by TAP consumers)
        # so the console output alone identifies the failing case.
        {
            grep -E '\*\*\*(Failed|Not Run|Timeout|Exception)|^ *FAIL:|Error|error:' "$log" |
                tail -20
            tail -20 "$log"
        } | sed 's/^/# /'
        assert_fail "$name host ctest" \
            "$(tail -20 "$log") [full log: test/results/logs/host-$name.log]"
    fi
}

# tool_suite <name> <command...> — same accounting and same per-suite log as
# suite(), for a host test that is not a CMake/ctest tree. The build-time Python
# tools have no CMakeLists to point suite() at, and their tests need no compiler.
tool_suite() {
    local name="$1"; shift
    local log="$LOGDIR/host-$name.log"
    if "$@" >"$log" 2>&1; then
        assert_pass "$name host test"
    else
        tail -20 "$log" | sed 's/^/# /'
        assert_fail "$name host test" \
            "$(tail -20 "$log") [full log: test/results/logs/host-$name.log]"
    fi
}

aup_key_build() {
    local b="$1" s="$2" k="$3"
    shift 3
    rm -rf "$b" &&
        cmake -S "$s" -B "$b" -DAUP_VERIFY_PUBKEY_PEM="$k" "$@" &&
        cmake --build "$b" --parallel "$HOST_TESTS_JOBS" --target mc_mgmt &&
        python3 -c 'import sys; sys.exit(open(sys.argv[2], "rb").read()
            not in open(sys.argv[1], "rb").read())' "$b/libmc_mgmt.a" "$k"
}

pic_key_build() {
    local b="$1" s="$2" k="$3"
    shift 3
    rm -rf "$b" &&
        cmake -S "$s" -B "$b" -DPIC_VERIFY_PUBKEY_PEM="$k" "$@" &&
        cmake --build "$b" --parallel "$HOST_TESTS_JOBS" --target core_miner &&
        python3 -c 'import sys; sys.exit(open(sys.argv[2], "rb").read()
            not in open(sys.argv[1], "rb").read())' "$b/core_miner" "$k"
}

verify_build() {
    local b="$1" s="$2" ver
    shift 2
    ver="$(sed -n 's/^set(MINER_PROTO_VER "\([^"]*\)".*/\1/p' "$s/cmake/MinerProtoVer.cmake")"
    if [[ ! "$ver" =~ ^[0-9]+\.[0-9]+$ ]]; then
        echo "MinerProtoVer.cmake sets no MINER_PROTO_VER <major>.<minor>: '$ver'" >&2
        return 1
    fi
    rm -rf "$b" &&
        cmake -S "$s" -B "$b" "$@" &&
        cmake --build "$b" --parallel "$HOST_TESTS_JOBS" --target core_miner &&
        grep -q "NEEDED.*libcrypto" < <(readelf -d "$b/core_miner") &&
        grep -qF "PVer[$ver]" "$b/core_miner"
}

plugin_stub_build() {
    local b="$1" s="$2"
    shift 2
    rm -rf "$b" &&
        cmake -S "$s" -B "$b" "$@" &&
        cmake --build "$b" --parallel "$HOST_TESTS_JOBS" --target core_miner &&
        grep -q plugin-stub-wp "$b/core_miner"
}

tool_suite "conf_check" "$HERE/conf_check.sh"
tool_suite "shellcheck" "$HERE/shellcheck.sh"
suite "common" common/tests
suite "osif" osif -DOSIF_BUILD_TESTS=ON
suite "hal" hal/tests
suite "miner_core" miner_core/tests "${OSSL_ARGS[@]}"
suite "miner_mgmt" miner_mgmt/tests "${OSSL_ARGS[@]}" "${LVGL_ARGS[@]}"
suite "miner-web" ../miner-web/tests "${OSSL_ARGS[@]}"
suite "harness" tests/harness
suite "sha256-hwcmp" tools/sha256-hwcmp
tool_suite "mc_mgmt-aup-key" aup_key_build "$MINER/build/aup-key" "$MINER" \
    "$MINER/miner_mgmt/tests/fixtures/aup-test-key.pub.pem" "${OSSL_ARGS[@]}" -DMINER_CORE_ONLY=ON \
    -DBOARD_DESC="$SDK_ROOT/product/k230/miner/avalon-home/nano3s/conf/product/boardconf.ini"
tool_suite "core_miner-pic-key" pic_key_build "$MINER/build/pic-key" "$MINER" \
    "$MINER/miner_mgmt/tests/fixtures/aup-test-key.pub.pem" "${OSSL_ARGS[@]}" -DMINER_CORE_ONLY=ON \
    -DBOARD_DESC="$SDK_ROOT/product/k230/miner/avalon-home/nano3s/conf/product/boardconf.ini"
# The default core_miner of the product links the libcrypto signature verifier
# and reports the protocol version MinerProtoVer.cmake sets.
verify() {
    local board="$1" desc="$2"; shift 2
    tool_suite "core_miner-verify-$board" verify_build "$MINER/build/verify-$board" "$MINER" \
        "${OSSL_ARGS[@]}" \
        -DBOARD_DESC="$SDK_ROOT/product/k230/$desc/conf/product/boardconf.ini" "$@"
}
verify nano3s miner/avalon-home/nano3s -DMINER_CORE_ONLY=ON
# The product must also build with an API plugin; tests/api_plugin_stub is
# a minimal one.
tool_suite "core_miner-plugin-stub" plugin_stub_build "$MINER/build/plugin-stub" "$MINER" \
    "${OSSL_ARGS[@]}" -DMINER_CORE_ONLY=ON \
    -DAPI_PLUGIN=ON -DAPI_PLUGIN_DIR="$MINER/tests/api_plugin_stub" \
    -DBOARD_DESC="$SDK_ROOT/product/k230/miner/avalon-home/nano3s/conf/product/boardconf.ini"

tap_done
