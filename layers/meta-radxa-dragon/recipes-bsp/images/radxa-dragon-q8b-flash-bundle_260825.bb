SUMMARY = "Radxa Dragon Q8B BIOS and minimal OS flash bundle"
DESCRIPTION = "Auditable deployment bundle containing separate SPI BIOS recovery files and 512-byte/UFS operating-system images."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://RADXA-DRAGON-Q8B-FLASHING.md"
S = "${UNPACKDIR}"

inherit deploy

COMPATIBLE_MACHINE = "^radxa-dragon-q8b$"
PACKAGE_ARCH = "${MACHINE_ARCH}"

Q8B_SD_IMAGE = "qcom-minimal-efi-sd-image-${MACHINE}.rootfs"
Q8B_UFS_IMAGE = "qcom-minimal-efi-ufs-4k-image-${MACHINE}.rootfs"
Q8B_BIOS_DIR = "radxa-dragon-q8b-bios-${PV}"
Q8B_BUNDLE_DIR = "radxa-dragon-q8b-flash-bundle-${PV}"

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy[depends] += " \
    firmware-radxa-dragon-q8b-bios:do_deploy \
    qcom-minimal-efi-sd-image:do_image_complete \
    qcom-minimal-efi-ufs-4k-image:do_image_complete \
"
do_deploy[cleandirs] = "${DEPLOYDIR}/${Q8B_BUNDLE_DIR}"
do_deploy() {
    bundle="${DEPLOYDIR}/${Q8B_BUNDLE_DIR}"
    install -d "$bundle/bios" "$bundle/images"

    test -d "${DEPLOY_DIR_IMAGE}/${Q8B_BIOS_DIR}/flat_build/spinor/dragon-q8b"
    cp -a "${DEPLOY_DIR_IMAGE}/${Q8B_BIOS_DIR}/flat_build" "$bundle/bios/"

    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q8B_SD_IMAGE}.wic" \
        "$bundle/images/q8b-512.wic"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q8B_SD_IMAGE}.wic.bmap" \
        "$bundle/images/q8b-512.wic.bmap"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q8B_UFS_IMAGE}.wic" \
        "$bundle/images/q8b-ufs-4k.wic"
    install -m 0644 "${DEPLOY_DIR_IMAGE}/${Q8B_UFS_IMAGE}.wic.bmap" \
        "$bundle/images/q8b-ufs-4k.wic.bmap"
    install -m 0644 "${UNPACKDIR}/RADXA-DRAGON-Q8B-FLASHING.md" \
        "$bundle/README.md"

    printf '%s\n' \
        '{' \
        '  "schema": 1,' \
        '  "machine": "${MACHINE}",' \
        '  "kernel_version": "7.0.11-6-qcom",' \
        '  "kernel_source_revision": "657c0f722940cd9d3b51abfa7383655ec7d2c795",' \
        '  "kernel_packaging_revision": "69f6ffe75b23f31312fc94b1b7f748929d41d9c9",' \
        '  "firmware_version": "0.2.41",' \
        '  "firmware_source_revision": "e1761009df008adfd62c77f2c5584e3067449013",' \
        '  "bios_version": "${PV}",' \
        '  "bios_source_sha256": "e233f879d2de19d0c0aa6c9042b5f402c1d606634d02cf75150a980e24cd1974",' \
        '  "images": {' \
        '    "sector_512": "images/q8b-512.wic",' \
        '    "ufs_sector_4096": "images/q8b-ufs-4k.wic"' \
        '  }' \
        '}' > "$bundle/manifest.json"

    (
        cd "$bundle"
        find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | \
            xargs -0 sha256sum > SHA256SUMS
        sha256sum -c SHA256SUMS
    )

    ln -sfn "${Q8B_BUNDLE_DIR}" \
        "${DEPLOYDIR}/radxa-dragon-q8b-flash-bundle"
}

addtask deploy after do_unpack before do_build
