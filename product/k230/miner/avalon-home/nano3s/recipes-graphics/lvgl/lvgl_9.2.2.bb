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

SUMMARY = "LVGL graphics library"
DESCRIPTION = "LVGL 9.2.2 with the Canaan lv_conf.h, rendering through the fbdev driver."
HOMEPAGE = "https://lvgl.io/"
LICENSE = "MIT & Zlib & BSD-3-Clause & PD & OFL-1.1"
LIC_FILES_CHKSUM = " \
    file://LICENCE.txt;md5=bf1198c89ae87f043108cea62460b03a \
    file://src/libs/lodepng/lodepng.c;beginline=1;endline=24;md5=c37031fc73a9420c96ab615745cd9034 \
    file://src/stdlib/builtin/lv_tlsf.h;beginline=18;endline=42;md5=bef65db1379e1efabcda3e30460858b7 \
    file://src/stdlib/builtin/lv_sprintf_builtin.c;beginline=2;endline=23;md5=fb197eb90a676c6ade4085036fba3a6a \
    file://src/misc/lv_math.c;beginline=79;endline=108;md5=94fac2fb2aa065b3b82940b9163d76be \
    file://${CANAAN_SDK_ROOT}/NOTICE;beginline=106;endline=110;md5=f1345a6d12bf501965d1a9cacfc4f9e1 \
    file://${CANAAN_SDK_ROOT}/third-party-license/OFL-1.1.txt;md5=f86799f77eaad418e28f2700062fc515 \
"

# Archive of tag v9.2.2.
LVGL_COMMIT = "7f07a129e8d77f4984fff8e623fd5be18ff42e74"
SRC_URI = " \
    https://github.com/lvgl/lvgl/archive/${LVGL_COMMIT}.tar.gz;downloadfilename=lvgl-${PV}-7f07a129.tar.gz \
    file://lv_conf.h \
    file://0001-fbdev-open-framebuffer-close-on-exec.patch \
    file://0002-fbdev-create-the-display-at-240x240.patch \
"
SRC_URI[sha256sum] = "8a5e30223fd5b08cdeccb060f45b37a29868d6b1fdfc5bdbdf0dd89aaefb5a0e"
S = "${WORKDIR}/lvgl-${LVGL_COMMIT}"

inherit cmake

do_configure:prepend() {
    cp ${WORKDIR}/lv_conf.h ${S}/lv_conf.h
}

# lv_conf.h comes through the relative include; LV_CONF_PATH breaks on the "linux" token in
# the path.
EXTRA_OECMAKE = " \
    -DBUILD_SHARED_LIBS=ON \
    -DLIB_INSTALL_DIR=${baselib} \
    -DLV_CONF_INCLUDE_SIMPLE=OFF \
    -DLV_CONF_BUILD_DISABLE_EXAMPLES=1 \
    -DLV_CONF_BUILD_DISABLE_DEMOS=1 \
    -DLV_CONF_BUILD_DISABLE_THORVG_INTERNAL=1 \
"

# Install lv_conf.h on the include root for LV_CONF_INCLUDE_SIMPLE consumers.
do_install:append() {
    install -m 0644 ${WORKDIR}/lv_conf.h ${D}${includedir}/lv_conf.h
}

FILES:${PN} += "${libdir}/liblvgl*.so.*"
FILES:${PN}-dev += "${includedir}/lvgl ${includedir}/lv_conf.h ${libdir}/liblvgl*.so"

INSANE_SKIP:${PN} += "dev-so"
