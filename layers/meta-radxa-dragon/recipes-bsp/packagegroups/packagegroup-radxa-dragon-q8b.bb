SUMMARY = "Packages for the Radxa Dragon Q8B platform"

inherit packagegroup

PACKAGES = " \
    ${PN}-firmware \
    ${PN}-hexagon-dsp-binaries \
"

RRECOMMENDS:${PN}-firmware = " \
    radxa-firmware-sc8280xp-firmware \
    linux-firmware-qcom-sc8280xp-lenovo-x13s-adreno \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wifi', 'linux-firmware-ath11k-qca2066 linux-firmware-ath11k-wcn6855 linux-firmware-ath12k-wcn7850 wireless-regdb-static', '', d)} \
    ${@bb.utils.contains('DISTRO_FEATURES', 'bluetooth', 'linux-firmware-qca-qca2066 linux-firmware-qca-wcn685x linux-firmware-qca-wcn7850', '', d)} \
"

RDEPENDS:${PN}-hexagon-dsp-binaries = " \
    radxa-firmware-sc8280xp-adsp \
    radxa-firmware-sc8280xp-cdsp \
"
