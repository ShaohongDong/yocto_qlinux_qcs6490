DESCRIPTION = "Tiny ramdisk image with SM8750 MTP firmware files"

PACKAGE_INSTALL += " \
    packagegroup-sm8750-mtp-firmware \
"

require initramfs-firmware-image.inc
