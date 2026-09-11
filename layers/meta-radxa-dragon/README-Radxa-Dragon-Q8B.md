# Radxa Dragon Q8B Yocto Usage Guide

OTA is enabled by default for `qcom-distro`. See [OTA operation and release
instructions](README-OTA.md). OTA images use versioned OSTree UKIs and mount
their ESP at `/boot`. The standalone ESP/UKI layout below applies when
`QCOM_OTA_ENABLE = "0"` is explicitly selected. The board audit detects both
layouts from the generated artifacts.

This guide builds EFI-based Yocto images for the Radxa Dragon Q8B (SC8280XP).
The machine uses Radxa `linux-qcom` 7.0.11-6, Radxa firmware 0.2.41 and the
260825 SPI NOR BIOS recovery release. It has no fallback to Linux 6.18.

The boot chain and UEFI are stored in SPI NOR. The disk images begin with an
EFI System Partition and do not contain a `qcomflash` boot-firmware payload.

## Select the machine

The SDK keeps Q6A as its default. Select Q8B for the current shell without
editing that default:

```sh
source ./environment-setup-armv8a-qcom-linux
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS MACHINE"
export MACHINE=radxa-dragon-q8b
```

## Build images

Build the minimal 512-byte-sector SD/NVMe and 4096-byte-sector UFS images:

```sh
layers/oe-core/bitbake/bin/bitbake qcom-minimal-efi-sd-image
layers/oe-core/bitbake/bin/bitbake qcom-minimal-efi-ufs-4k-image
```

Build the proprietary multimedia variants when the corresponding Qualcomm
packages are required:

```sh
layers/oe-core/bitbake/bin/bitbake qcom-multimedia-proprietary-efi-sd-image
layers/oe-core/bitbake/bin/bitbake qcom-multimedia-proprietary-efi-ufs-4k-image
```

Build the auditable minimal-image and BIOS bundle:

```sh
layers/oe-core/bitbake/bin/bitbake radxa-dragon-q8b-flash-bundle
```

Artifacts are deployed under:

```text
tmp/deploy/images/radxa-dragon-q8b/
```

## Offline validation

After building the flash bundle, audit GPT layouts, bmaps, DTB/UKI identity,
required rootfs firmware, BIOS XML references and bundle checksums:

```sh
layers/meta-radxa-dragon/scripts/audit-radxa-dragon-q8b.py \
  tmp/deploy/images/radxa-dragon-q8b
```

This audit does not write a device. Successful offline checks do not establish
that boot, display, PCIe, camera, audio, networking or acceleration works on a
physical board.

## Flashing references

- [Q8B downloads and BIOS](https://docs.radxa.com/en/dragon/q8b/download)
- [Install to microSD](https://docs.radxa.com/en/dragon/q8b/getting-started/install-system/sd-system)
- [Install to NVMe](https://docs.radxa.com/en/dragon/q8b/getting-started/install-system/nvme-system)
- [Install to UFS](https://docs.radxa.com/en/dragon/q8b/getting-started/install-system/ufs-system)

Always resolve and inspect the exact destination block device before writing
an image. BIOS recovery and device flashing are intentionally outside the
offline build procedure.
