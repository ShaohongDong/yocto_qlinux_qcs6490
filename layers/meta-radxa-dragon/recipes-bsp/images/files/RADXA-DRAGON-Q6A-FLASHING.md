# Radxa Dragon Q6A flash bundle

This bundle deliberately separates the SPI NOR BIOS from the operating-system
disk images. Verify `SHA256SUMS` before using any file. The commands below are
examples only; inspect the target and obtain explicit authorization before
writing a device.

## Contents

- `bios/flat_build/spinor/dragon-q6a/`: official Radxa SPI NOR recovery payload.
- `images/q6a-512.wic`: 512-byte-sector image for microSD, eMMC, or NVMe.
- `images/q6a-512.wic.bmap`: block map for the 512-byte-sector image.
- `images/q6a-ufs-4k.wic`: 4096-byte-sector UFS image.
- `images/q6a-ufs-4k.wic.bmap`: block map for the UFS image.
- `manifest.json`: pinned source and build identities.

## Recover the SPI NOR BIOS

From `bios/flat_build/spinor/dragon-q6a/`, with the board in EDL mode:

```sh
sudo edl-ng --memory=spinor rawprogram rawprogram0.xml patch0.xml \
  --loader=prog_firehose_ddr.elf
```

Do not erase or rewrite SPI NOR during a normal OS installation.

## Write an OS image through EDL

For an eMMC module in the SDCC slot:

```sh
sudo edl-ng --loader bios/flat_build/spinor/dragon-q6a/prog_firehose_ddr.elf \
  --memory Sdcc --slot 0 write-sector 0 images/q6a-512.wic
```

For a UFS module:

```sh
sudo edl-ng --loader bios/flat_build/spinor/dragon-q6a/prog_firehose_ddr.elf \
  --memory ufs write-sector 0 images/q6a-ufs-4k.wic
```

For removable media, prefer a card reader and `bmaptool copy` with the matching
`.bmap` file. Never infer the destination device name from these examples.
