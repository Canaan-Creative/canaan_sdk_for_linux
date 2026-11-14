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

# PACKAGE_FEED_URIS is empty, so update-apt-sources-list.sh is dropped.
INITSCRIPT_PACKAGES = ""

do_install:append() {
    rm -f ${D}${INIT_D_DIR}/update-apt-sources-list.sh
}
