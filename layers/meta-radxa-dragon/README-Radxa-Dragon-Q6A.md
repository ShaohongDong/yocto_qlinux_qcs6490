# Radxa Dragon Q6A Yocto Usage Guide

OTA is enabled by default for `qcom-distro`. See [OTA operation and release
instructions](README-OTA.md). OTA images retain embloader but use OSTree
deployment paths, a `/boot` ESP, signed updates and boot-attempt counting.
The fixed `RadxaOS/<kernel-release>` ESP layout described below applies when
`QCOM_OTA_ENABLE = "0"` is explicitly selected.

This guide builds the EFI-based Yocto images for the Radxa Dragon Q6A and writes
them to a microSD card or UFS module.

The machine is pinned to Radxa `linux-qcom` 7.0.11-6. There is no Q6A fallback
to the layer's 6.18 recipe; rebuild external kernel modules after upgrading.

Q6A uses source-built embloader 0.7 at `EFI/BOOT/BOOTAA64.EFI`. Its BLS entry
under `loader/entries/` loads the matching raw kernel, initramfs and board DTB
from `RadxaOS/<kernel-release>/`. The machine selects this layout through
`QCOM_ESP_IMAGE`; other machines retain their default ESP provider.

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

For an eSDK with locked signatures, include `kmod kmod-native` in
`SIGGEN_UNLOCKED_RECIPES` in `conf/unlocked-sigs.inc`. This allows the Zstd
module support to be rebuilt instead of reusing the original SDK tools.

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

## Common Maintenance Tools

The Q6A `qcom-multimedia-proprietary-efi-sd-image` installs
`packagegroup-radxa-dragon-common-tools` for maintenance and board diagnostics.
Other images do not automatically include this package group.

| Purpose | Tools and examples |
| --- | --- |
| Network interfaces and routes | `ifconfig -a`, `ip addr`, `ip route`, `ss -lnt`, `netstat -rn` |
| Connectivity and DNS | `ping`, `arping`, `tracepath`, `traceroute`, `dig`, `host`, `nslookup` |
| Network diagnostics | `ethtool`, `iw`, `iperf3`, `tcpdump` |
| Transfers | `curl`, `wget`, `rsync`; CA certificates included |
| Process diagnostics | `htop`, `lsof`, `strace`, `ps`, `file`, `less` |
| Editing and shell sessions | `nano`, `tmux`, `jq`, GNU `find` and `diff` |
| Archives | `tar`, `gzip`, `bzip2`, `xz`, `zip`, `unzip` |
| Board diagnostics | `lsusb`, `lspci`, `i2cdetect -l`, `picocom` |

DNS utilities are client tools; this package group does not request the BIND
server or a compiler toolchain. Command links shared with BusyBox are managed
by the recipes' alternatives mechanism.

Build from the eSDK root:

```bash
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 devtool build-image qcom-multimedia-proprietary-efi-sd-image
```

Validate the completed manifest and command files in the generated rootfs.
Offline inspection confirms tool inclusion, not physical network, serial, or
I2C operation; those require separate board validation.

For an OTA release, select a version newer than the running deployment and
preserve its application selection using a release-specific BitBake config:

```bitbake
QCOM_OTA_VERSION = "2.0.8"
QCOM_APP = "app003-hevc-benchmark"
QCOM_APP_IMAGE = "qcom-multimedia-proprietary-efi-sd-image"
```

Pass that config with `bitbake -R /path/to/release.conf` after activating the
eSDK, then follow [the signed OTA workflow](README-OTA.md). Verify the packaged
initramfs against the standalone deploy artifact before publishing; a cached
boot payload must not silently replace the current one.

The 2026-09-19 Q6A OTA 2.0.8 validation passed image structural checks, 96
executable/library file hash checks, and 35 board smoke checks. The image added
27 packages without removing existing packages; WIC size increased by 52 MiB.
Board checks covered command availability, Ethernet connectivity, DNS, HTTP,
packet capture, file/archive operations, tmux, iperf3 loopback, USB/PCI/I2C
enumeration and XFCE readiness. OTA boot confirmation passed with 2.0.7 retained
as rollback. Serial data transfer, I2C transactions and physical power-loss
rollback were not tested. Three PipeWire failed units were present before the
update; no failed systemd units remained after reboot. Audio functionality was
not tested.

## Write BIOS

The board must have a compatible BIOS/UEFI release in SPI NOR. Obtain it from
[Radxa Dragon Q6A BIOS](https://dl.radxa.com/users/dev/radxa-dragon/qli-2.0/radxa-dragon-q6a/dragon-q6a_flat_build_wp_260815.zip).

- [Flashing BIOS Firmware](https://docs.radxa.com/en/dragon/q6a/low-level-dev/spi-fw)

## Write an Image

- [Install the system on a microSD card](https://docs.radxa.com/en/dragon/q6a/getting-started/install-system/sd-system)
- [Install the system on a UFS module with a card reader](https://docs.radxa.com/en/dragon/q6a/getting-started/install-system/ufs-system/ufs-reader-system)

## Maintainers

Radxa Dev <dev@radxa.com>
