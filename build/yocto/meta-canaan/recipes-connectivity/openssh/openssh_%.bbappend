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

SRC_URI += "https://git.openembedded.org/openembedded-core/plain/meta/recipes-connectivity/openssh/openssh/CVE-2024-6387.patch?id=ddb998d16fd869acb00a1cd8038ada20fd32aa8b;downloadfilename=openssh-CVE-2024-6387.patch;name=cve-2024-6387"
SRC_URI[cve-2024-6387.sha256sum] = "b0ff779d1a87fd2f4aa84f4361de4c4fcf21d996148293d3c82c4c700277745b"

do_install:append() {
    rm -f ${D}${sysconfdir}/ssh/ssh_host_*
}
