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

# build.sh exports both versions; the defaults serve a plain bitbake run.
CANAAN_SDK_VERSION ?= "0.0.0"
CANAAN_MINER_VERSION ?= "0.0.0"
CANAAN_IMG_VERSION = "sdk${CANAAN_SDK_VERSION}_miner${CANAAN_MINER_VERSION}"

PACKAGE_ARCH = "${MACHINE_ARCH}"

ID:k230-nano3s = "canaan-nano3s"
NAME:k230-nano3s = "Canaan Nano3s"
VERSION:k230-nano3s = "${CANAAN_IMG_VERSION}"
VERSION_ID:k230-nano3s = "${CANAAN_SDK_VERSION}_${CANAAN_MINER_VERSION}"
PRETTY_NAME:k230-nano3s = "Canaan Nano3s ${CANAAN_IMG_VERSION}"
HOME_URL:k230-nano3s = "https://www.canaan.io/"
