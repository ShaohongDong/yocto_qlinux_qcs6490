# SPDX-License-Identifier: MIT
# Pinned QCS6490 reference configuration, validated for Q6A stereo headphones.
# Only that device is exposed by the Q6A PipeWire integration.
PACKAGE_ARCH:radxa-dragon-q6a = "${MACHINE_ARCH}"
do_install:append:radxa-dragon-q6a() {
    for kind in mixer_paths resourcemanager; do
        install -m 0644 ${S}/configs/qcom/IoT/qcs6490/${kind}_QCS6490_RB3Gen2.xml \
            ${D}${sysconfdir}/${kind}_QCS6490_Radxa_Dragon_Q6A.xml
    done
}
