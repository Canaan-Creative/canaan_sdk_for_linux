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

# miner-web.init supervise: a miner_web that exits is started again after a
# doubling delay, and SIGTERM stops the supervisor and miner_web.
# Usage: test_init.sh <path to miner-web.init> [shell, default sh]
INIT=$1
SH=${2:-sh}
dir=$(mktemp -d) || exit 1
trap 'rm -rf "$dir"' EXIT
fails=0

# check <description> <command...>
check() {
    desc=$1
    shift
    if "$@"; then
        echo "  ok: $desc"
    else
        echo "  FAIL: $desc"
        fails=$((fails + 1))
    fi
}

# within <tenths of a second> <command...>: wait until the command succeeds
within() {
    n=$1
    shift
    i=0
    while [ "$i" -lt "$n" ]; do
        "$@" && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

started() {
    [ -f "$dir/starts" ] && [ "$(wc -l < "$dir/starts")" -ge "$1" ]
}

started_only() {
    [ "$(wc -l < "$dir/starts")" -eq "$1" ]
}

logged() {
    grep -q "$1" "$dir/miner.log"
}

alive() {
    kill -0 "$1" 2>/dev/null
}

gone() {
    ! kill -0 "$1" 2>/dev/null
}

# Fake miner_web: records its pid, exits 3 unless $dir/stay exists.
cat > "$dir/fake" <<EOF
#!/bin/sh
echo \$\$ >> "$dir/starts"
[ -f "$dir/stay" ] && exec sleep 1000
exit 3
EOF
chmod +x "$dir/fake"

DAEMON="$dir/fake" LOG_DIR="$dir" $SH "$INIT" supervise &
sup=$!

check "a miner_web that exits is started again" within 50 started 2
check "...and the exit is logged with its status and the delay" \
    logged "miner_web exited (status 3); restarting in 3 s"
check "...the delay doubles while it keeps exiting" \
    within 20 logged "restarting in 6 s"

touch "$dir/stay"
check "...and it is started after that delay" within 80 started 3
web=$(tail -n 1 "$dir/starts")
check "...and stays up" within 20 alive "$web"

kill -TERM "$sup"
check "SIGTERM stops the supervisor" within 20 gone "$sup"
check "...and the miner_web it runs" within 20 gone "$web"
check "...and nothing is started again" started_only 3

if [ "$fails" -ne 0 ]; then
    echo "FAILED ($fails)"
    exit 1
fi
echo OK
