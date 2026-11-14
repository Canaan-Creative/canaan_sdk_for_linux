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

# Let the given/when build scripts compile under Perl 5.38 with fatal warnings.
python do_patch:append() {
    import os
    s = d.getVar('S')
    old = "no  if $] >= 5.018, warnings => 'experimental::smartmatch';"
    new = old + "\nno  if $] >= 5.036, warnings => 'deprecated';" + "\nuse feature 'switch';"
    for fname in ['build-aux/scripts/BuildCommon.pm', 'build-aux/scripts/gen-crypt-h']:
        path = os.path.join(s, fname)
        with open(path) as f:
            content = f.read()
        with open(path, 'w') as f:
            f.write(content.replace(old, new, 1))
}

EXTRA_OECONF:append = " --enable-hashes=strong,glibc"
