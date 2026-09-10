# SPDX-License-Identifier: MIT
SUMMARY = "Embedded UEFI boot loader"
HOMEPAGE = "https://github.com/BigfootACA/embloader"
LICENSE = "GPL-2.0-or-later & MIT & BSD-2-Clause & BSD-3-Clause & BSD-2-Clause-Patent & Apache-2.0 & Zlib"
LIC_FILES_CHKSUM = " \
    file://debian/copyright;md5=5bc8f163808cbe6568b27e6aabeb99b2 \
    file://edk2/License.txt;md5=2b415520383f7964e96700ae12b4570a \
    file://embloader/ext/libufdt/NOTICE;md5=3f52ca43505196dea390350987d6a022 \
    file://embloader/ext/nanosvg/LICENSE.txt;md5=078690812af4ba8567fcc2af2ca1d307 \
"

# Fetch each pinned submodule explicitly: compilation must never update Git.
SRC_URI = " \
    git://github.com/BigfootACA/embloader.git;protocol=https;nobranch=1;name=main;destsuffix=${BP} \
    git://github.com/tianocore/edk2.git;protocol=https;nobranch=1;name=edk2;destsuffix=${BP}/edk2 \
    git://github.com/google/brotli.git;protocol=https;nobranch=1;name=brotli;destsuffix=${BP}/edk2/BaseTools/Source/C/BrotliCompress/brotli \
    git://github.com/MIPI-Alliance/public-mipi-sys-t.git;protocol=https;nobranch=1;name=mipisyst;destsuffix=${BP}/edk2/MdePkg/Library/MipiSysTLib/mipisyst \
    git://git.kernel.org/pub/scm/utils/dtc/dtc.git;protocol=https;nobranch=1;name=dtc;destsuffix=${BP}/embloader/ext/dtc \
    git://github.com/json-c/json-c.git;protocol=https;nobranch=1;name=jsonc;destsuffix=${BP}/embloader/ext/json-c \
    git://android.googlesource.com/platform/system/libufdt;protocol=https;nobranch=1;name=libufdt;destsuffix=${BP}/embloader/ext/libufdt \
    git://github.com/yaml/libyaml.git;protocol=https;nobranch=1;name=libyaml;destsuffix=${BP}/embloader/ext/libyaml \
    git://github.com/BigfootACA/lvgl.git;protocol=https;nobranch=1;name=lvgl;destsuffix=${BP}/embloader/ext/lvgl \
    git://github.com/memononen/nanosvg.git;protocol=https;nobranch=1;name=nanosvg;destsuffix=${BP}/embloader/ext/nanosvg \
    git://sourceware.org/git/newlib-cygwin.git;protocol=https;nobranch=1;name=newlib;destsuffix=${BP}/embloader/ext/newlib \
    git://github.com/nothings/stb.git;protocol=https;nobranch=1;name=stb;destsuffix=${BP}/embloader/ext/stb \
"
SRCREV_main = "f3a03df73b142e59c4fa2e2b7b68cf3fca73a8fe"
SRCREV_edk2 = "d46aa46c8361194521391aa581593e556c707c6e"
SRCREV_brotli = "f4153a09f87cbb9c826d8fc12c74642bb2d879ea"
SRCREV_mipisyst = "370b5944c046bab043dd8b133727b2135af7747a"
SRCREV_dtc = "9197f1ccd95c9475006677aa97bf39727c1e8aa5"
SRCREV_jsonc = "2372e9518e6ba95b48d37ec162bc7d93b297b52f"
SRCREV_libufdt = "9b70f594cf149c7c73989c78f03d210f8aa1c1f6"
SRCREV_libyaml = "840b65c40675e2d06bf40405ad3f12dec7f35923"
SRCREV_lvgl = "4668ed768fd5d7187bf9eb836bf8b274f6345ab4"
SRCREV_nanosvg = "ea6a6aca009422bba0dbad4c80df6e6ba0c82183"
SRCREV_newlib = "3a03874f73db015d93c3b54c6bad32a2852c591e"
SRCREV_stb = "f58f558c120e9b32c217290b80bad1a0729fbb2c"
SRCREV_FORMAT = "main_edk2_brotli_mipisyst_dtc_jsonc_libufdt_libyaml_lvgl_nanosvg_newlib_stb"
B = "${WORKDIR}/build"

inherit deploy python3native
COMPATIBLE_HOST = "aarch64.*-linux"
DEPENDS = "nasm-native acpica-native util-linux-native"

do_patch[postfuncs] += "patch_embloader_submodules"
patch_embloader_submodules() {
    cd ${S}
    bash scripts/patch.sh
}

do_configure() {
    # EDK2 does not consume OE's CFLAGS; map __FILE__ paths explicitly.
    sed -i 's| -Wno-char-subscripts.*$| -Wno-char-subscripts -ffile-prefix-map=${WORKDIR}=/usr/src/debug/${PN}/${PV}|' \
        ${S}/embloader/embloader.dsc
}

do_compile() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS ARCH
    oe_runmake -C ${S}/edk2/BaseTools \
        CC="${BUILD_CC}" CXX="${BUILD_CXX}" AR="${BUILD_AR}" \
        BUILD_CC="${BUILD_CC}"
    export WORKSPACE=${B}
    export CONF_PATH=${B}/Conf
    install -d ${B}/Conf
    export PACKAGES_PATH=${S}/edk2:${S}
    export EDK_TOOLS_PATH=${S}/edk2/BaseTools
    export GCC5_AARCH64_PREFIX=${TARGET_PREFIX}
    export PYTHON_COMMAND=${PYTHON}
    cd ${S}
    bash -c '. edk2/edksetup.sh && build -a AARCH64 -t GCC5 -b RELEASE \
        -n ${@oe.utils.parallel_make(d)} \
        -D DISABLE_NEW_DEPRECATED_INTERFACES=TRUE \
        -D EMBLOADER_VERSION=\"${PV}\" -p embloader/embloader.dsc'
    # Release EFI images must not embed the build directory in CodeView data.
    ${S}/edk2/BaseTools/Source/C/bin/GenFw --zero --replace \
        ${B}/Build/embloader/RELEASE_GCC5/AARCH64/embloader.efi
}

do_install() {
    install -Dm0644 ${B}/Build/embloader/RELEASE_GCC5/AARCH64/embloader.efi \
        ${D}${datadir}/embloader/embloader.efi
}
do_deploy() {
    install -Dm0644 ${D}${datadir}/embloader/embloader.efi ${DEPLOYDIR}/embloader-${PV}.efi
}
addtask deploy after do_install before do_build
FILES:${PN} = "${datadir}/embloader"
