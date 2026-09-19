# SPDX-License-Identifier: MIT
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:radxa-dragon-q6a = " file://0001-agm-fix-ats-thread-scheduling.patch file://0002-agm-align-card-name-with-pal.patch"
PACKAGE_ARCH:radxa-dragon-q6a = "${MACHINE_ARCH}"
