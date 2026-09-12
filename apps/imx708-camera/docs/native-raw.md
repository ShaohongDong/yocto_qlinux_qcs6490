# Native CAM3 RAW diagnostic

This path uses Linux `imx708`, GPIO I2C and CAMSS; it does not enable the CamX
backend in `app.yaml`. It targets an IMX708 module on Q6A CAM3 with module-local
24 MHz clock and internal sensor supplies. It is not a general bare-sensor
power configuration. Keep native CCI disabled until its lockup is diagnosed.

## Prepare an independent device tree

Use the **original deployed Q6A DTB with symbols**, not a previous diagnostic
DTB. The preparation tool refuses to overwrite an output or change the input.

```sh
python3 apps/imx708-camera/scripts/prepare-raw-dtb.py \
    --base original.dtb --output raw-bus.dtb --stage bus
python3 apps/imx708-camera/scripts/prepare-raw-dtb.py \
    --base original.dtb --output raw-sensor.dtb --stage sensor
python3 apps/imx708-camera/scripts/prepare-raw-dtb.py \
    --base original.dtb --output raw-camss.dtb --stage camss
```

Stages enable GPIO I2C only, then the sensor, then CAMSS. SDA/SCL use TLMM
75/76 open drain. Module enable is TLMM78, logical high for sensor power-on.
CAMSS analog rails are Q6A L10C and L6B. The sensor endpoint uses clock lane 0
and data lanes 1/2; the CAMSS endpoint uses hardware clock lane 7 and data
lanes 0/1. Do not copy sensor lane numbers into the receiver endpoint.

## Boot containment and staged probing

Preserve the original DTB and BLS entry. Copy each diagnostic DTB to a separate
ESP path. Derive a separate `ostree-raw-<stage>+1.conf` from the current normal
BLS entry, retaining its kernel, initramfs and OSTree root. Change only title,
DTB path and the diagnostic options. For this embloader, add
`sort-key 00-q6a-raw` to select the diagnostic ahead of ordinary entries
without sort keys. A large `version` alone does not establish that order.

Remove `quiet` and append:

```text
q6a.raw_diag=<stage> systemd.mask=qcom-ota-confirm.service systemd.mask=systemd-bless-boot.service modprobe.blacklist=imx708,qcom_camss,i2c_qcom_cci
```

Before enabling camera hardware, verify this arrangement using the original
DTB: the diagnostic boot must consume the attempt (`+0-1.conf`), the services
must be masked, and a second reboot must skip it and select the normal entry.
Keep serial capture running throughout. An exhausted attempt provides fallback
on the next boot; it does not automatically reboot a hung kernel.

For the bus stage, identify the adapter by `i2c-cam3-gpio` in sysfs, rather than
assuming a bus number. Read IMX708 chip ID at register 0x0016, expected 0x0708.
Only manipulate GPIO78 when no kernel sensor driver owns it. Do not scan/write
the autofocus controller or EEPROM to test video reception.

For the sensor stage, manually `modprobe imx708`, inspect probe logs and verify
bounded unbind/bind cycles. The CAMSS stage then manually loads `qcom-camss`.
Stop at the first failed stage and preserve serial and kernel logs.

## Capture and inspect

Run the capture script on the board with media-ctl, v4l2-ctl, stdbuf and Python:

```sh
python3 capture-native-raw.py --output /var/tmp/raw-bars --frames 4
python3 capture-native-raw.py --output /var/tmp/raw-scene --frames 100 --test-pattern 0
```

It configures `imx708 -> msm_csiphy3 -> msm_csid0 -> msm_vfe0_rdi0` for
2304x1296 SRGGB10, captured as packed `pRAA`. It records the actual stride,
sizeimage, per-buffer sequences and monotonic EOF timestamps. A 60-second
timeout, error buffer, missing frame, non-monotonic timestamp or wrong byte
count fails the capture; incomplete results remain explicitly marked false.
Capture completion verifies transport, not optical response or image quality.

Copy the complete capture directory to an analysis host with numpy and Pillow:

```sh
python3 apps/imx708-camera/scripts/preview-native-raw.py raw-scene \
    --output raw-preview
```

The preview preserves native 10-bit values in `bayer10.png` and combines each
RGGB cell into a half-size RGB PNG. No automatic exposure, white balance,
autofocus or ISP tuning is applied; a dark preview is possible. Test sensor
colour bars first, then real-scene response to covering and uncovering the lens,
100 complete frames, and three independent stream start/stop cycles. Preserve
captures and logs in ignored `artifacts/`; do not add them to the app package.

At completion, archive the exhausted diagnostic BLS entries outside
`/boot/loader/entries` before rebooting normally. Preserve the original normal
entries. The OTA confirmer checks all entries matching the running deployment;
leaving diagnostic `+0-1` entries pointing at that same deployment causes its
missing-boot-count check to fail on normal boot. If fallback has already booted,
archive the exhausted diagnostic entries and restart `qcom-ota-confirm.service`.
Verify normal boot selection, OTA confirmation, service health and disabled
native camera nodes. Production integration and CamX acceptance remain separate
work.
