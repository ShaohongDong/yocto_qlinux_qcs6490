SUMMARY = "Radxa Dragon Q8B SPI NOR BIOS recovery firmware"
DESCRIPTION = "Official Radxa Q8B flat-build payload for recovering the SPI NOR boot chain with EDL-NG."
HOMEPAGE = "https://docs.radxa.com/en/dragon/q8b/download"

LICENSE = "CLOSED"

SRC_URI = "https://dl.radxa.com/dragon/q8b/images/dragon-q8b_flat_build_wp_260825.zip"
SRC_URI[sha256sum] = "e233f879d2de19d0c0aa6c9042b5f402c1d606634d02cf75150a980e24cd1974"

S = "${UNPACKDIR}/flat_build/spinor/dragon-q8b"

inherit deploy

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy[cleandirs] = "${DEPLOYDIR}/radxa-dragon-q8b-bios-${PV}"
do_deploy() {
    target="${DEPLOYDIR}/radxa-dragon-q8b-bios-${PV}/flat_build/spinor/dragon-q8b"
    install -d "$target"

    for source in "${S}"/*; do
        if [ -f "$source" ]; then
            install -m 0644 "$source" "$target/"
        fi
    done
    chmod 0755 "$target/prog_firehose_ddr.elf" "$target/prog_firehose_lite.elf"

    for required in prog_firehose_ddr.elf prog_firehose_lite.elf \
                    rawprogram0.xml patch0.xml gpt_main0.bin gpt_backup0.bin \
                    xbl.elf xbl_config.elf imagefv.elf tz.mbn hyp.mbn \
                    aop.mbn cpucp.elf shrm.elf qupv3fw.elf \
                    usb4_uc_fw_image.elf eth_fw.bin; do
        test -s "$target/$required"
    done

    (
        cd "${DEPLOYDIR}/radxa-dragon-q8b-bios-${PV}"
        find flat_build -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS
        sha256sum -c SHA256SUMS
    )

    ln -sfn "radxa-dragon-q8b-bios-${PV}" \
        "${DEPLOYDIR}/radxa-dragon-q8b-bios"
}

addtask deploy after do_unpack before do_build
