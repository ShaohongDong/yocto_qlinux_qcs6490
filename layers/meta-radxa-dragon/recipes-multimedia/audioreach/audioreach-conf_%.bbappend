# SPDX-License-Identifier: MIT
# Upstream installs each platform to /etc/card-defs.xml; the last one wins.
# Select the QCS/QCM6490 virtual PCM definitions deterministically and copy the pinned QCS6490 reference graph for the board-verified stereo
# headphone path; other devices/use cases are not enabled by this integration.
PACKAGE_ARCH:radxa-dragon-q6a = "${MACHINE_ARCH}"
do_install:append:radxa-dragon-q6a() {
    install -m 0644 ${S}/qcom/qli/qcm6490/card-defs.xml ${D}${sysconfdir}/card-defs.xml
    install -d ${D}${sysconfdir}/acdbdata/QCS6490_Radxa_Dragon_Q6A
    install -m 0644 ${S}/qcom/qli/qcm6490/acdbdata/* \
        ${D}${sysconfdir}/acdbdata/QCS6490_Radxa_Dragon_Q6A/
}
