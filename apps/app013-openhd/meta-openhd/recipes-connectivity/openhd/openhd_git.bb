# SPDX-License-Identifier: MIT
SUMMARY = "Pinned upstream OpenHD baseline for Q6A development"
HOMEPAGE = "https://github.com/OpenHD/OpenHD"
LICENSE = "GPL-3.0-only & OpenHD-Non-Military"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464 file://OpenHD/main.cpp;beginline=1;endline=22;md5=0af1d12bfb67576cce2892b42f3d7f6e"
PV = "2.7+gitf07729b35e27"
SRC_URI = " \
    git://github.com/OpenHD/OpenHD;protocol=https;nobranch=1;name=core;destsuffix=core \
    git://github.com/nlohmann/json;protocol=https;nobranch=1;name=json;destsuffix=json \
    git://github.com/gabime/spdlog;protocol=https;nobranch=1;name=spdlog;destsuffix=spdlog \
    git://github.com/OpenHD/wifibroadcast;protocol=https;nobranch=1;name=wifibroadcast;destsuffix=wifibroadcast \
    git://github.com/OpenHD/mavlink-headers;protocol=https;nobranch=1;name=mavlink_headers;destsuffix=mavlink-headers \
    file://0001-Include-cstdint-for-MavlinkSystem.patch \
"
SRCREV_core = "f07729b35e273fe3612e1aade030a7a86350d1ac"
SRCREV_json = "904592d2a86bae5d9c191cdcb8f8fd9b136f71fa"
SRCREV_spdlog = "fc7e9c8721b95661cc40e2d1d2329677d2a21387"
SRCREV_wifibroadcast = "14d5fdb1b49a715529a39ce3583dae1e45776aeb"
SRCREV_mavlink_headers = "effd5bf95fad1ca524c6f1babde827cfb602f7fb"
SRCREV_FORMAT = "core_json_spdlog_wifibroadcast_mavlink_headers"
S = "${UNPACKDIR}/core"
OECMAKE_SOURCEPATH = "${S}/OpenHD"
DEPENDS = "poco libsodium libpcap gstreamer1.0 gstreamer1.0-plugins-base libusb1 v4l-utils python3-native"
inherit cmake pkgconfig python3native
EXTRA_OECMAKE = "-DENABLE_AIR=ON -DENABLE_USB_CAMERAS=ON"
# Build the application rather than upstream's manual hardware test programs.
OECMAKE_TARGET_COMPILE = "openhd"

# Explicit Git revisions freeze every submodule without recursive moving fetches.
python do_unpack:append() {
    import os
    import shutil
    submodules = (
        ("json", "OpenHD/ohd_common/lib/json"),
        ("spdlog", "OpenHD/ohd_common/lib/spdlog"),
        ("wifibroadcast", "OpenHD/ohd_interface/lib/wifibroadcast"),
        ("mavlink-headers", "OpenHD/ohd_telemetry/lib/mavlink-headers"),
    )
    for source, destination in submodules:
        target = os.path.join(d.getVar("S"), destination)
        shutil.rmtree(target)
        shutil.copytree(os.path.join(d.getVar("UNPACKDIR"), source), target)
}

# No service is installed or enabled while radio/platform qualification is pending.
RDEPENDS:${PN} += "iw iproute2 networkmanager-nmcli"
COMPATIBLE_MACHINE = "${@'^radxa-dragon-q6a$' if d.getVar('QCOM_APP') == 'app013-openhd' else '^$'}"
