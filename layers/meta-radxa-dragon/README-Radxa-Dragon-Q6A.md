# Radxa Dragon Q6A Yocto Usage Guide

This guide builds the EFI-based Yocto images for the Radxa Dragon Q6A and writes
them to a microSD card or UFS module.

> The Q6A boots from the Qualcomm boot chain and UEFI stored in SPI NOR. The
> images produced here start at the EFI System Partition; they do not contain a
> qcomflash boot-firmware payload.

## Prerequisites

- A Linux host with Git and [Kas](https://kas.readthedocs.io/) 5.x installed.
- Enough free disk space for a Yocto build (at least 100 GiB is recommended).
- A Radxa Dragon Q6A with current SPI NOR BIOS/UEFI firmware.
- A microSD card or a UFS module and compatible card reader.

## Get the Layer

Clone the Q6A layer on the supported branch:

```bash
git clone --branch wrynose https://github.com/radxa/meta-radxa-dragon.git meta-qcom
```

Run the commands below from the workspace that contains `meta-qcom`.

## Build Images

Set the shared Kas configuration once:

```bash
KAS_CONFIG=meta-qcom/ci/radxa-dragon-q6a.yml:meta-qcom/ci/qcom-distro.yml
```

### microSD image

```bash
kas build --target qcom-multimedia-proprietary-efi-sd-image "$KAS_CONFIG"
```

### UFS image

```bash
kas build --target qcom-multimedia-proprietary-efi-ufs-4k-image "$KAS_CONFIG"
```

### Minimal images in the Qualcomm extensible SDK

When this repository replaces the SDK's bundled `meta-qcom` layer, select the
Q6A machine in `conf/local.conf` and build the two minimal images explicitly:

```bash
source ./environment-setup-armv8a-qcom-linux
devtool build-image qcom-minimal-efi-sd-image
devtool build-image qcom-minimal-efi-ufs-4k-image
```

The optional `radxa-dragon-q6a-flash-bundle` target downloads and verifies the
matching SPI NOR recovery firmware and collects both OS images, their block
maps, checksums, and safe flashing notes. The SDK does not expose BitBake on
`PATH`, so invoke its bundled executable directly:

```bash
layers/oe-core/bitbake/bin/bitbake radxa-dragon-q6a-flash-bundle
```

Audit the completed deploy directory without writing any device:

```bash
layers/meta-radxa-dragon/scripts/audit-radxa-dragon-q6a.py \
  tmp/deploy/images/radxa-dragon-q6a
```

## Locate Build Artifacts

All generated artifacts are placed in:

```text
build/tmp/deploy/images/radxa-dragon-q6a/
```

The primary disk images are:

```text
qcom-multimedia-proprietary-efi-sd-image-radxa-dragon-q6a.rootfs.wic
qcom-multimedia-proprietary-efi-ufs-4k-image-radxa-dragon-q6a.rootfs.wic
```

## Write BIOS

The board must have a compatible BIOS/UEFI release in SPI NOR. Obtain it from
[Radxa Dragon Q6A BIOS](https://dl.radxa.com/users/dev/radxa-dragon/qli-2.0/radxa-dragon-q6a/dragon-q6a_flat_build_wp_260815.zip).

- [Flashing BIOS Firmware](https://docs.radxa.com/en/dragon/q6a/low-level-dev/spi-fw)

## Write an Image

- [Install the system on a microSD card](https://docs.radxa.com/en/dragon/q6a/getting-started/install-system/sd-system)
- [Install the system on a UFS module with a card reader](https://docs.radxa.com/en/dragon/q6a/getting-started/install-system/ufs-system/ufs-reader-system)

## Maintainers

Radxa Dev <dev@radxa.com>
