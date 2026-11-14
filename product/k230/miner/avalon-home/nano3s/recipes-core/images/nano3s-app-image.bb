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

DESCRIPTION = "Avalon Nano3s miner app volume: the /mntapp/release tree for app_a/app_b."
LICENSE = "MIT"

# App volume; gen-spinand.sh packs its mntapp/release subtree into app.ubifs.
IMAGE_INSTALL = "avalon-miner-core avalon-miner-config"

export IMAGE_BASENAME = "nano3s-app-image"

inherit core-image
