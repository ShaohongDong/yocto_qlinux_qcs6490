# Hardware ISP bring-up boundary

## Confirmed inputs

- Machine `radxa-dragon-q6a`; kernel `linux-qcom` 7.0.11, SRCREV
  `657c0f722940cd9d3b51abfa7383655ec7d2c795`.
- `drivers/media/i2c/imx708.c` exists and the built config has
  `CONFIG_VIDEO_IMX708=m` and `CONFIG_VIDEO_QCOM_CAMSS=m`.
- CamX driver recipe 1.0.5 uses
  `qualcomm-linux/camera-driver@0f16924ff6a7f9bb56a7e958016da2ed8a174f2f`.
- Kodiak CamX/CHI 1.0.25 archives are available in `downloads/`. CHI ships
  IMX577 and other sensor/module/tuning binaries, but no IMX708 assets or
  sensor XML compiler. `camxcommon-headers` is not a complete CHI-CDK.
- The application uses camera-service 1.0.5 APIs and IMSDK 1.0.2
  (`806ad865287c418d2aac380506b7ad44b5a11319`), including `capture-image`,
  `video-metadata`, `static-metadata` and the documented enum nicknames.
  It deliberately avoids the plugin's `static-metas` pointer table: that
  revision populates it with addresses into a local vector.

## CAM3 wiring reference

The [Radxa Q6A V1.21 schematic](https://dl.radxa.com/dragon/q6a/hw/radxa_dragon_q6a_schematic_v1.21.pdf),
CAM sheet (printed sheet 34, PDF page 32), shows CAM3 at connector **J7**:

| Pins | Connection |
| --- | --- |
| 2, 3 | CSI3 data lane 0 negative, positive |
| 5, 6 | CSI3 data lane 1 negative, positive |
| 8, 9 | CSI3 clock negative, positive |
| 11 | CAM3_RESET_3V3 through U19 level shifter |
| 12 | CAM_MCLK3_3V3 through R121, marked not fitted |
| 13, 14 | CCI_I2C3 SCL, SDA through U19 |
| 15 | 3.3 V |

This is a connector reference, not a verified cable assignment. Confirm the
actual board revision and FPC orientation/continuity before applying power
sequences. Camera Module 3's onboard oscillator and regulator arrangement
must be reflected in the CamX sensor power description; do not copy another
sensor's MCLK or regulator settings. The user's "120 degree" description is
recorded as Camera Module 3 Wide, not used as a calibrated optical parameter.

## Missing prerequisites and required integration

Update: reference CHI headers/schema/compiler, authored module/actuator XML and
a compilable CAM3 DT candidate are now available. The [material-based report](materials-progress.md)
records actual ABI and v2/v7 format mismatches; reference inputs must not be
confused with missing inputs or with verified target-compatible assets.

1. Obtain a CHI-CDK with sensor driver ABI headers, matching XML schemas and
   binary-generation tool compatible with Kodiak CamX 1.0.25, plus tuning
   generation support. Public Qualcomm camera-service and IMSDK repositories
   provide client APIs, not these IMX708 assets. No compatible public IMX708
   CamX package was found in this investigation.
2. Port IMX708 register modes and exposure/gain calculations to that ABI;
   use the pinned [register reference](register-reference.md) supplied by
   the user, including its dynamic startup writes and CAM3 two-lane selection.
   The [source-built core](sensor-core.md) implements register/control logic;
   its CHI ABI wrapper is still required. Create module and autofocus actuator descriptions for the Wide module.
   Validate power sequencing and lens limits against the actual module.
   Linux's V4L2 driver is useful register documentation, not a CamX sensor
   library. Do not rename or substitute the supplied IMX577 assets.
3. Create Q6A CAM3 proprietary camera DT nodes against the actual camera-driver
   bindings; the [platform audit](cam3-bindings.md) identifies the missing
   proprietary platform nodes and the CCI1/master1/CSIPHY3 mapping. Verify all
   resources, including CSI3, CCI3, reset, IOMMU, clocks and regulators. Verify
   the current `kodiak.dtsi` platform resources against that driver. Ensure
   native CAMSS/CCI and CamX do not claim the same hardware. Stage the final
   patch and config through this app's `kernel` manifest entries only once
   this binding is established; no such patch has been validated yet.
4. Generate and package sensor/module/actuator/tuning artifacts with source
   revisions and checksums, and use Wide-module calibration data for AF/AE/AWB.
   Build the driver and DTB, then verify runtime ABI/module loading on Q6A.
5. Test HDMI preview and H.264 hardware encoding at 1080p30 for 30 minutes,
   full-size JPEG, near/far AF, AE/AWB convergence, repeated record/stop,
   service failure, disk-full and application exit. Keep logs, sample files,
   sensor identity, frame timestamps and dropped-frame counts. Image creation
   and synthetic tests cannot mark any of these hardware gates passed.

No media flashing, reboot or connected-board test is part of the offline work.
