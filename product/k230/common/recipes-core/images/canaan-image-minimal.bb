# SPDX-License-Identifier: Apache-2.0
# Modified by Canaan Creative Co., Ltd. from xuantie-yocto
# meta-xuantie/recipes-core/images/xuantie-image-minimal.bb.
DESCRIPTION = "A minimal image for Canaan K230."
LICENSE = "Apache-2.0"

IMAGE_INSTALL = "packagegroup-core-boot"
EXTRA_IMAGEDEPENDS += "u-boot"

TOOLCHAIN_TARGET_TASK:remove = " target-sdk-provides-dummy"

python do_rootfs:append:riscv64() {
    import subprocess

    srcdir=d.getVar('IMAGE_ROOTFS')

    imagename = 'IMAGE_BASENAME={0}\n'.format(d.getVar('IMAGE_BASENAME'))
    with open('%s%s/os-release' % (srcdir, d.getVar('nonarch_libdir')), 'a+') as f:
        f.seek(0)
        lines = f.read()
        if imagename.strip() not in lines:
            f.write(imagename)

    if 'systemd' not in d.getVar('DISTRO_FEATURES', True).split():
        systemddir = srcdir + '/usr/lib/systemd/'
        if systemddir and os.path.exists(systemddir):
            import shutil
            shutil.rmtree(systemddir)
}

DISTRO_FEATURES:remove = " ipv6 pcmcia usbgadget usbhost pci 3g nfc vfat "

export IMAGE_BASENAME = "canaan-image-minimal"

inherit core-image
