# SPDX-License-Identifier: MIT
SUMMARY = "IMX708 two-lane register/control core and offline sequence inspector"
DESCRIPTION = "Source-built register data and exposure calculations. This is not a CHI sensor plugin."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = " \
    file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6 \
    file://imx708-fac260a.c;beginline=1;endline=8;md5=2e243fab666c127546892cde23f63028 \
"

SRC_URI = " \
    https://raw.githubusercontent.com/6by9/linux/fac260a153218b4f0897a7974068ff77e2b17b8b/drivers/media/i2c/imx708.c;downloadfilename=imx708-fac260a.c;name=reference \
    file://CMakeLists.txt \
    file://generate-registers.py \
    file://sensor.hpp \
    file://sensor.cpp \
    file://dump.cpp \
    file://chi_adapter.cpp \
    file://chi_test.cpp \
    file://sensor_test.cpp \
    file://test_generator.py \
"
SRC_URI[reference.sha256sum] = "d83d25e985faacd8029a7dcc98be9c749e060953b292335337b2e43df9a58704"
S = "${UNPACKDIR}"
inherit cmake python3native
EXTRA_OECMAKE = "-DIMX708_REFERENCE_SOURCE=${S}/imx708-fac260a.c -DBUILD_TESTING=OFF"
COMPATIBLE_MACHINE = "^radxa-dragon-q6a$"
