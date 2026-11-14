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

# Writes /etc/release_version and an init script that prints it at boot.

# Set by build.sh; a bare bitbake run records these defaults.
CANAAN_GIT_REV ?= "unknown"
CANAAN_SDK_VERSION ?= "0.0.0"
CANAAN_MINER_VERSION ?= "0.0.0"

# Set only by images that install the miner.
CANAAN_RELEASE_MINER_VERSION ?= ""

# YYYYMMDDHHmm from build.sh, shared with the image filename and the miner binaries.
MINER_BUILD_TIME ?= ""

# oe-core's rootfs_reproducible writes /etc/version as a file.
CANAAN_RELEASE_VERSION_FILE ?= "${sysconfdir}/release_version"
CANAAN_RELEASE_INITSCRIPT ?= "${CANAAN_SDK_ROOT}/product/k230/common/files/release-version.init"

ROOTFS_POSTPROCESS_COMMAND += "canaan_write_release_version; "
do_rootfs[file-checksums] += "${CANAAN_RELEASE_INITSCRIPT}:True"

canaan_write_release_version() {
    # Without MINER_BUILD_TIME, use the local clock.
    if [ -n "${MINER_BUILD_TIME}" ]; then
        build_time="$(echo "${MINER_BUILD_TIME}" |
            sed -E 's/^(....)(..)(..)(..)(..)$/\1-\2-\3 \4:\5:00/')"
    else
        build_time="$(date +'%Y-%m-%d %H:%M:%S')"
    fi
    cat > ${IMAGE_ROOTFS}${CANAAN_RELEASE_VERSION_FILE} <<EOF
# Rootfs build identity, written by the image recipe at do_rootfs time.
# sh-sourceable. BUILD_TIME is local time, the same value the image filename and
# the miner binaries carry. GIT_COMMIT is the SDK repo HEAD this rootfs
# was built from, 8 characters: the first 8 hex digits, or the first 7 and a
# "t" when tracked files other than submodules had uncommitted changes. "t" is not a hex digit,
# so a dirty build cannot be misread as a commit id.
MACHINE="${MACHINE}"
IMAGE="${IMAGE_BASENAME}"
SDK_VERSION="${CANAAN_SDK_VERSION}"
MINER_VERSION="${CANAAN_RELEASE_MINER_VERSION}"
BUILD_TIME="$build_time"
GIT_COMMIT="${CANAAN_GIT_REV}"
EOF
    chmod 0644 ${IMAGE_ROOTFS}${CANAAN_RELEASE_VERSION_FILE}

    # S02 prints the identity right after the boot banner.
    install -d ${IMAGE_ROOTFS}${sysconfdir}/init.d ${IMAGE_ROOTFS}${sysconfdir}/rcS.d
    install -m 0755 ${CANAAN_RELEASE_INITSCRIPT} \
        ${IMAGE_ROOTFS}${sysconfdir}/init.d/release-version
    ln -sf ../init.d/release-version \
        ${IMAGE_ROOTFS}${sysconfdir}/rcS.d/S02release-version
}
