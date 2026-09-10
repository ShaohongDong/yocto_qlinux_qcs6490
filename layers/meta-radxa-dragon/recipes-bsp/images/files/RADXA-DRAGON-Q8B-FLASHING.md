# Radxa Dragon Q8B flash bundle

This bundle keeps the SPI NOR BIOS separate from the operating-system images.
Verify `SHA256SUMS` before using any file. The commands below are examples;
inspect the target and obtain explicit authorization before writing a device.

## Contents

- `bios/flat_build/spinor/dragon-q8b/`: official Radxa SPI NOR recovery payload.
- `images/q8b-512.wic`: 512-byte-sector image for microSD or NVMe media.
- `images/q8b-512.wic.bmap`: block map for the 512-byte-sector image.
- `images/q8b-ufs-4k.wic`: 4096-byte-sector UFS image.
- `images/q8b-ufs-4k.wic.bmap`: block map for the UFS image.
- `manifest.json`: pinned kernel, firmware, BIOS and image identities.

## Recover the SPI NOR BIOS

From `bios/flat_build/spinor/dragon-q8b/`, with the board in EDL mode:

```sh
sudo edl-ng --memory=spinor rawprogram rawprogram0.xml patch0.xml \
  --loader=prog_firehose_ddr.elf
```

Do not erase or rewrite SPI NOR during a normal OS installation.

## Write an OS image

For a UFS module through EDL:

```sh
sudo edl-ng --loader bios/flat_build/spinor/dragon-q8b/prog_firehose_ddr.elf \
  --memory ufs write-sector 0 images/q8b-ufs-4k.wic
```

For microSD or NVMe media, use a card reader or enclosure and `bmaptool copy`
with `images/q8b-512.wic` and its matching `.bmap`. Never infer the destination
device name from an example.
