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

# Runs shellcheck on every tracked shell script of this repository: *.sh, *.bash
# and any file whose first line is a sh/bash shebang. Runs at -S warning with
# SC2034 (unused variable), SC1090 (non-constant source) and SC2046 (unquoted
# command substitution) excluded.
# One assertion per script.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$HERE/../.." && pwd)"
source "$HERE/lib/assert.sh"
tap_init "shellcheck" "$SDK_ROOT/test/results/shellcheck.xml"

SHEBANG='^#![[:space:]]*(/usr/bin/env[[:space:]]+|/usr/bin/|/bin/)(ba|da|k)?sh([[:space:]]|$)'

if ! command -v shellcheck >/dev/null 2>&1; then
    assert_fail "shellcheck installed" "shellcheck is not on the PATH"
    tap_done
    exit 1
fi
if ! files="$(git -C "$SDK_ROOT" ls-files)"; then
    assert_fail "list tracked files" "$SDK_ROOT is not a git checkout"
    tap_done
    exit 1
fi

scripts=()
while IFS= read -r f; do
    [[ -f "$SDK_ROOT/$f" ]] || continue
    case "$f" in
    *.sh|*.bash) scripts+=("$f") ;;
    *) head -n1 "$SDK_ROOT/$f" 2>/dev/null | grep -qaE "$SHEBANG" && scripts+=("$f") ;;
    esac
done <<<"$files"

for f in "${scripts[@]}"; do
    if out="$(cd "$SDK_ROOT" && shellcheck -S warning -e SC2034,SC1090,SC2046 "$f" 2>&1)"; then
        assert_pass "$f"
    else
        printf '%s\n' "$out" | sed 's/^/# /'
        assert_fail "$f" "$out"
    fi
done

tap_done
