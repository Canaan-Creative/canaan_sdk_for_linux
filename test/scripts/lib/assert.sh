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

# TAP-ish assertion + result accounting, shared by the test scripts.
# Emits "ok N - desc" / "not ok N - desc" to stdout and a JUnit XML at $RESULTS_XML.
# Source this, then call: assert_pass/assert_fail/assert_xfail and the assert_* helpers.
#
# Usage:
#   source "$(dirname "$0")/lib/assert.sh"
#   tap_init "conf-check" "test/results/conf-check.xml"
#   assert_true "[[ -f foo ]]" "foo exists"
#   assert_le 7 8 "linux_system.bin fits 8M slot"
#   tap_done   # prints plan, writes JUnit, exits nonzero if any hard failure

set -uo pipefail

_TAP_N=0
_TAP_FAIL=0
# Skips and xfails render as "ok" (TAP has no other spelling for them), so they
# are counted apart from passes: a suite that skipped everything must not report
# itself as fully passed.
_TAP_SKIP=0
_TAP_XFAIL=0
_TAP_SUITE=""
_TAP_CASES="" # accumulates JUnit <testcase> lines
RESULTS_XML="${RESULTS_XML:-}"

tap_init() {
    _TAP_SUITE="${1:-tests}"
    RESULTS_XML="${2:-$RESULTS_XML}"
    _TAP_N=0; _TAP_FAIL=0; _TAP_SKIP=0; _TAP_XFAIL=0; _TAP_CASES=""
    echo "# suite: $_TAP_SUITE"
}

_xml_escape() { sed -e 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }

_record() { # $1=status(ok|fail|xfail|skip) $2=desc $3=detail
    _TAP_N=$((_TAP_N+1))
    local desc; desc="$(printf '%s' "$2" | _xml_escape)"
    local detail; detail="$(printf '%s' "${3:-}" | _xml_escape)"
    case "$1" in
        ok)
            echo "ok $_TAP_N - $2"
            _TAP_CASES+="<testcase name=\"$desc\"/>"
            ;;
        fail)
            echo "not ok $_TAP_N - $2"
            _TAP_FAIL=$((_TAP_FAIL+1))
            _TAP_CASES+="<testcase name=\"$desc\"><failure>$detail</failure></testcase>"
            ;;
        xfail)
            echo "ok $_TAP_N - $2 # XFAIL ${3:-}" # expected-fail: known gap, not a hard fail
            _TAP_XFAIL=$((_TAP_XFAIL+1))
            _TAP_CASES+="<testcase name=\"$desc\"><skipped>xfail: $detail</skipped></testcase>"
            ;;
        skip)
            echo "ok $_TAP_N - $2 # SKIP ${3:-}"
            _TAP_SKIP=$((_TAP_SKIP+1))
            _TAP_CASES+="<testcase name=\"$desc\"><skipped>$detail</skipped></testcase>"
            ;;
    esac
}

assert_pass() { _record ok "$1"; }
assert_fail() { _record fail "$1" "${2:-}"; }
assert_xfail() { _record xfail "$1" "${2:-known gap}"; }
assert_skip() { _record skip "$1" "${2:-}"; }

assert_true() {
    if eval "$1"; then _record ok "$2"; else _record fail "$2" "cond failed: $1"; fi
}
assert_le() {
    if [[ "$1" -le "$2" ]]; then _record ok "$3"; else _record fail "$3" "$1 > $2"; fi
}
assert_exists() {
    if [[ -e "$1" ]]; then _record ok "$2"; else _record fail "$2" "missing: $1"; fi
}
assert_absent() {
    if [[ ! -e "$1" ]]; then _record ok "$2"; else _record fail "$2" "should be absent: $1"; fi
}

tap_done() {
    echo "1..$_TAP_N"
    echo "# $((_TAP_N-_TAP_FAIL-_TAP_SKIP-_TAP_XFAIL))/$_TAP_N passed, $_TAP_FAIL failed," \
        "$_TAP_SKIP skipped, $_TAP_XFAIL xfail"
    if [[ -n "$RESULTS_XML" ]]; then
        mkdir -p "$(dirname "$RESULTS_XML")"
        {
            echo "<?xml version=\"1.0\"?>"
            echo "<testsuite name=\"$_TAP_SUITE\" tests=\"$_TAP_N\" failures=\"$_TAP_FAIL\"" \
                "skipped=\"$((_TAP_SKIP+_TAP_XFAIL))\">"
            echo "$_TAP_CASES"
            echo "</testsuite>"
        } > "$RESULTS_XML"
    fi
    [[ "$_TAP_FAIL" -eq 0 ]]
}
