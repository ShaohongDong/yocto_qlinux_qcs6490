# Source-built IMX708 register core

`imx708-sensor-core` is a separate GPL-2.0-only recipe. It builds the register
tables and control calculations from source and installs an offline inspector
plus development library/header. It is not a CHI plugin and is not linked
into the MIT GUI application. Its source is under
`apps/recipes-camera/imx708-sensor-core/files/`.

## Reproducible builds

In the SDK shell:

```sh
layers/oe-core/bitbake/bin/bitbake imx708-sensor-core
```

The recipe fetches the pinned 6by9 driver, checks its SHA-256, and generates
ordered C++ arrays with the original register value widths. The generator
rejects changed input rather than accepting an unreviewed branch update.

For host validation, supply the same source file:

```sh
cmake -S apps/recipes-camera/imx708-sensor-core/files -B /tmp/imx708-core \
  -DIMX708_REFERENCE_SOURCE=/path/to/pinned/imx708.c
cmake --build /tmp/imx708-core
ctest --test-dir /tmp/imx708-core --output-on-failure
/tmp/imx708-core/imx708-register-dump preview spc-calibrated
/tmp/imx708-core/imx708-register-dump still spc-default
```

The inspector accesses no device. Its explicit SPC argument selects the
conditional startup branch for review; a real backend must read `0x7b10`
successfully and select the matching branch. Stream-on is deliberately
separate from the register plan so the backend can complete CSI/ISP setup
before enabling transmission.

## Implemented interface

`timing()` describes preview/still modes. `calculate()` reports the actual
representable frame length, exposure and gains; `controls()` produces writes;
`startup()` combines verified reference tables with dynamic two-lane settings;
`encode()` serializes 16-bit addresses and 8/16-bit big-endian values.

Gain follows `1024 / (1024 - analog_code)` and a digital factor of
`digital_code / 256`. The analogue formula is independently documented in
[Raspberry Pi's IMX708 helper](https://github.com/raspberrypi/libcamera/blob/main/src/ipa/rpi/cam_helper/cam_helper_imx708.cpp).
Requests below the analogue minimum saturate to the minimum; the result
reports that actual gain. Above 16x analogue gain the remaining request uses
digital gain, capped at `0xffff`. Invalid/nonfinite gains and unsupported
frame/lane/link settings are rejected.

Long frames use the reference's shift mechanism (maximum shift 7); effective
frame/exposure values report quantization. Live updates still require the
matching CHI frame scheduling/group-hold interface. The library alone does
not perform I2C transactions, power sequencing, autofocus or ISP processing.

## Integration blockers

See the newer [material-based implementation report](materials-progress.md):
reference tools and a CHI callback adapter have since been built and exercised.
The following local SDK inventory does not describe all publicly obtainable inputs.

No compatible sensor ABI, XML schema/compiler or Wide tuning was found in
the local SDK search. The current Kodiak package is prebuilt.
[Critical Link's QC6490 guide](https://support.criticallink.com/redmine/projects/mitysom_qc6490/wiki/Camera_Config)
describes obtaining proprietary `meta-qcom-extras` and rebuilding `chicdk-kt`;
it does not distribute that source. Its older BSP is not proof of compatibility
with Kodiak CamX 1.0.25. The public
[InnoIPA release project](https://github.com/InnoIPA/iQ-Cam__manifest)
targets another BSP and does not establish a compatible Q6A development kit.

The image tasks for the two supported proprietary EFI images now stop when
`QCOM_APP = "imx708-camera"` until CHI and platform DT integration is supplied.
The application and register core can still be built independently. No
configuration switch turns an incomplete integration into a supported image.
See [CAM3 platform bindings](cam3-bindings.md) for the separate DT gap.
