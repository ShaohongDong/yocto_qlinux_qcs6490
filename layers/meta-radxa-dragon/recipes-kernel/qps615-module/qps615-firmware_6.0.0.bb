SUMMARY = "QPS615 PCIe Ethernet switch firmware"
DESCRIPTION = "Firmware binary for the QPS615 PCIe Ethernet switch"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${S}/Bin/LICENSE;md5=e8623ee428d33d107c80c8991f828bb7"

inherit allarch

SRCREV = "f9b0e1bc0f7c3dfc74ad1a46a87efa56885b9288"

SRC_URI = "git://github.com/TC956X/TC9564_Firmware.git;protocol=https;branch=industrial_limited_tested;tag=V6.0.0"

do_install() {
	install -d ${D}${nonarch_base_libdir}/firmware/
	install -m 0644 ${S}/Bin/TC956X_Firmware_PCIeBridge.bin ${D}${nonarch_base_libdir}/firmware/
}

FILES:${PN} += "${nonarch_base_libdir}/firmware/TC956X_Firmware_PCIeBridge.bin"
