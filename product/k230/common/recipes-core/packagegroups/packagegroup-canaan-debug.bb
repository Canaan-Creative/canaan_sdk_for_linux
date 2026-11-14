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

SUMMARY = "Canaan K230 on-target debug and diagnostic tools"
DESCRIPTION = "Command-line tools for diagnosing a board from an interactive shell, installed by"
DESCRIPTION += "the canaan-debug image feature."

LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    iperf3 \
    i2c-tools \
    memtester \
    strace \
    tcpdump \
    ethtool \
    iw \
    lsof \
    evtest \
    libgpiod-tools \
    spitools \
"
