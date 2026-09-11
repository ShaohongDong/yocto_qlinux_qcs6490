SUMMARY = "Radxa Dragon Q6A BIOS and minimal OS flash bundle"
DESCRIPTION = "Auditable deployment bundle containing separate SPI BIOS recovery files and 512-byte/UFS operating-system images."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://RADXA-DRAGON-Q6A-FLASHING.md"
S = "${UNPACKDIR}"

inherit deploy

COMPATIBLE_MACHINE = "^radxa-dragon-q6a$"
PACKAGE_ARCH = "${MACHINE_ARCH}"

Q6A_SD_IMAGE = "qcom-minimal-efi-sd-image-${MACHINE}.rootfs"
Q6A_UFS_IMAGE = "qcom-minimal-efi-ufs-4k-image-${MACHINE}.rootfs"
Q6A_BIOS_DIR = "radxa-dragon-q6a-bios-${PV}"
Q6A_BUNDLE_DIR = "radxa-dragon-q6a-flash-bundle-${PV}"

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy[depends] += " \
    firmware-radxa-dragon-q6a-bios:do_deploy \
    qcom-minimal-efi-sd-image:do_image_complete \
    qcom-minimal-efi-ufs-4k-image:do_image_complete \
"
do_deploy[cleandirs] = "${DEPLOYDIR}/${Q6A_BUNDLE_DIR}"
do_deploy() {
    bundle="${DEPLOYDIR}/${Q6A_BUNDLE_DIR}"
    install -d "$bundle/bios" "$bundle/images"

    test -d "${DEPLOY_DIR_IMAGE}/${Q6A_BIOS_DIR}/flat_build/spinor/dragon-q6a"
    cp -a "${DEPLOY_DIR_IMAGE}/${Q6A_BIOS_DIR}/flat_build" "$bundle/bios/"

    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q6A_SD_IMAGE}.wic" \
        "$bundle/images/q6a-512.wic"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q6A_SD_IMAGE}.wic.bmap" \
        "$bundle/images/q6a-512.wic.bmap"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q6A_UFS_IMAGE}.wic" \
        "$bundle/images/q6a-ufs-4k.wic"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q6A_UFS_IMAGE}.wic.bmap" \
        "$bundle/images/q6a-ufs-4k.wic.bmap"
    if [ "${QCOM_OTA_ENABLE}" = "1" ]; then
        install -d "$bundle/ota"
        for image in "${Q6A_SD_IMAGE}" "${Q6A_UFS_IMAGE}"; do
            install -m 0644 "${DEPLOY_DIR_IMAGE}/$image.ota.json" "$bundle/ota/"
            install -m 0644 "${DEPLOY_DIR_IMAGE}/$image.ostreecommit.tar.xz" "$bundle/ota/"
        done
    fi
    install -m 0644 "${UNPACKDIR}/RADXA-DRAGON-Q6A-FLASHING.md" \
        "$bundle/README.md"

    printf '%s\n' \
        '{' \
        '  "schema": 1,' \
        '  "machine": "${MACHINE}",' \
        '  "meta_radxa_dragon_commit": "bf84c2cc62a814eda9f03c1c2ccceb7c81fab127",' \
        '  "sdk_meta_qcom_base_commit": "ef0004df267743cc89d6a09bc724bc99ff541c01",' \
        '  "bios_version": "${PV}",' \
        '  "bios_source_sha256": "8d8e1c939913f1f6ee431e60fbc2c15afce82d8dd9257602fc622d4f05d721d7",' \
        '  "images": {' \
        '    "sector_512": "images/q6a-512.wic",' \
        '    "ufs_sector_4096": "images/q6a-ufs-4k.wic"' \
        '  }' \
        '}' > "$bundle/manifest.json"

    (
        cd "$bundle"
        find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | \
            xargs -0 sha256sum > SHA256SUMS
        sha256sum -c SHA256SUMS
    )

    ln -sfn "${Q6A_BUNDLE_DIR}" \
        "${DEPLOYDIR}/radxa-dragon-q6a-flash-bundle"
}

addtask deploy after do_unpack before do_build
