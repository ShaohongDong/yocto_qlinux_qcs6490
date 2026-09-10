DESCRIPTION = "Tiny ramdisk image with IQ-x7181 EVK firmware files"

PACKAGE_INSTALL += " \
    packagegroup-hamoa-iot-evk-firmware \
"

require initramfs-firmware-image.inc
