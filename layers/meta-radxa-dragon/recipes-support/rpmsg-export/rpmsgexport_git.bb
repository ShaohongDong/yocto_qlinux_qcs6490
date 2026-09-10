SUMMARY = "Qualcomm RPMSGEXPORT application"
HOMEPAGE = "https://github.com/linux-msm/rpmsgexport.git"
SECTION = "devel"

LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=ff273e1fd41fa52668171e0817c89724"

SRCREV = "ad7cc961b28a4b52b5178da9356c2f482a8d3e84"
SRC_URI = "git://github.com/linux-msm/${BPN}.git;branch=master;protocol=https"

inherit meson
