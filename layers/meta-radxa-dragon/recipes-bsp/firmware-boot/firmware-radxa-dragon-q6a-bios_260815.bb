SUMMARY = "Radxa Dragon Q6A SPI NOR BIOS recovery firmware"
DESCRIPTION = "Official Radxa Q6A flat-build payload for recovering the SPI NOR boot chain with EDL-NG."
HOMEPAGE = "https://docs.radxa.com/dragon/q6a/low-level-dev/spi-fw"

LICENSE = "CLOSED"

SRC_URI = "https://dl.radxa.com/users/dev/radxa-dragon/qli-2.0/radxa-dragon-q6a/dragon-q6a_flat_build_wp_260815.zip"
SRC_URI[sha256sum] = "8d8e1c939913f1f6ee431e60fbc2c15afce82d8dd9257602fc622d4f05d721d7"

S = "${UNPACKDIR}/flat_build/spinor/dragon-q6a"

inherit deploy

do_configure[noexec] = "1"
do_compile[noexec] = "1"
do_install[noexec] = "1"

do_deploy[cleandirs] = "${DEPLOYDIR}/radxa-dragon-q6a-bios-${PV}"
do_deploy() {
    target="${DEPLOYDIR}/radxa-dragon-q6a-bios-${PV}/flat_build/spinor/dragon-q6a"
    install -d "$target"

    for source in "${S}"/*; do
        if [ -f "$source" ]; then
            install -m 0644 "$source" "$target/"
        fi
    done
    chmod 0755 "$target/prog_firehose_ddr.elf" "$target/prog_firehose_lite.elf"

    for required in prog_firehose_ddr.elf rawprogram0.xml patch0.xml \
                    gpt_main0.bin gpt_backup0.bin xbl.elf xbl_config.elf \
                    imagefv.elf PILFV.Fv; do
        test -s "$target/$required"
    done

    (
        cd "${DEPLOYDIR}/radxa-dragon-q6a-bios-${PV}"
        find flat_build -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS
    )

    ln -sfn "radxa-dragon-q6a-bios-${PV}" \
        "${DEPLOYDIR}/radxa-dragon-q6a-bios"
}

addtask deploy after do_unpack before do_build
