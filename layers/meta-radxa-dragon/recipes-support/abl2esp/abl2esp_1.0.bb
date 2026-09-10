SUMMARY = "Minimal ABL reimplementation that chainloads bootaa64.efi from ESP on Qualcomm devices"
DESCRIPTION = "abl2esp is a minimal reimplementation of Qualcomm’s Android Bootloader (ABL). \
It scans all available filesystems for EFI/boot/bootaa64.efi and attempts to load and start it, \
enabling direct boot of an EFI loader (e.g., systemd-boot or GRUB) from the EFI System Partition"

LICENSE = "BSD-3-Clause-Clear"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=2998c54c288b081076c9af987bdf4838"

SRC_URI = "\
    git://github.com/qualcomm/abl2esp.git;branch=main;protocol=https \
    https://github.com/qualcomm/abl2esp/releases/download/v1.0/abl2esp-v5.elf;name=v5 \
    https://github.com/qualcomm/abl2esp/releases/download/v1.0/abl2esp-v6.elf;name=v6 \
    https://github.com/qualcomm/abl2esp/releases/download/v1.0/abl2esp-v7.elf;name=v7 \
"

SRC_URI[v5.sha256sum] = "098c03dff5f03e3f01f9e7852e260c65b8fd83494fbe38c3667128504fd5576b"
SRC_URI[v6.sha256sum] = "f51c98c7e2d7caee4735fc11760926b38ecd41ffc473db01775b335683e7eb94"
SRC_URI[v7.sha256sum] = "0e96701001770671e8ea48278a10f3672a015fcf262a3a563f3776248c069ac6"

SRCREV = "ab361526adb31d6c11034858d02b8d46dd6afa12"

inherit deploy

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy() {
        install -m 0644 ${UNPACKDIR}/abl2esp-v5.elf -D ${DEPLOYDIR}/
        install -m 0644 ${UNPACKDIR}/abl2esp-v6.elf -D ${DEPLOYDIR}/
        install -m 0644 ${UNPACKDIR}/abl2esp-v7.elf -D ${DEPLOYDIR}/
}
addtask deploy before do_build after do_unpack
