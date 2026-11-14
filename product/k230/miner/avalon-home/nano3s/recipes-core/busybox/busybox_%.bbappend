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

# Slim applet set, quiet init banner and a 1000-line shell history.

FILESEXTRAPATHS:prepend:k230-nano3s := "${THISDIR}/${PN}:"

SRC_URI:append:k230-nano3s = " \
    file://k230-nano3s-slim.cfg \
"

# Drop the per-service echo that meta-xuantie adds to rcS.
do_install:append:k230-nano3s() {
    if [ -f "${D}${sysconfdir}/init.d/rcS" ]; then
        sed -i '/echo "Starting service \$i"/d' ${D}${sysconfdir}/init.d/rcS
    fi
}
