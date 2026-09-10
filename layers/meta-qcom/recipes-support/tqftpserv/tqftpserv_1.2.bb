SUMMARY = "Qualcomm tqftpserv application"
HOMEPAGE = "https://github.com/linux-msm/tqftpserv.git"
SECTION = "devel"

LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=424e013ed97b36284f3b9ce27bb16a56"

DEPENDS = "qrtr zstd"

inherit systemd meson pkgconfig

SRCREV = "b6bb92d40cfffe28621abcf7bfaa6d99beea46cb"
SRC_URI = "git://github.com/linux-msm/${BPN}.git;branch=master;protocol=https;tag=v${PV} \
"

EXTRA_OEMESON = "-Dsystemd-unit-prefix=${systemd_system_unitdir}"

SYSTEMD_SERVICE:${PN} = "tqftpserv.service"
RDEPENDS:${PN} += "qrtr"
