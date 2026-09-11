# Material-based CHI and CAM3 implementation

No board is currently available. This does not prevent source adaptation,
component builds, XML validation, ABI inspection or DT compilation.

## Sources recovered

- Raspberry Pi Linux commit `50f88724518d2eafe75bfae7923e90a8fe171c66`:
  [module DTS](https://github.com/raspberrypi/linux/blob/50f88724518d2eafe75bfae7923e90a8fe171c66/arch/arm/boot/dts/overlays/imx708.dtsi)
  establishes IMX708 at 7-bit address `0x1a`, DW9817 at `0x0c`, 24 MHz,
  two CSI lanes and a 450 MHz link.
  [VCM driver](https://github.com/raspberrypi/linux/blob/50f88724518d2eafe75bfae7923e90a8fe171c66/drivers/media/i2c/dw9807-vcm.c)
  establishes 10-bit DAC, idle 512, initial 480, status register 5,
  DAC registers 3/4 and 16-code gradual moves.
- [Official camera specifications](https://www.raspberrypi.com/documentation/accessories/camera.html#hardware-specification):
  Wide optics are 2.75 mm, f/2.2, horizontal/vertical FoV 102/67 degrees,
  and focus from approximately 5 cm to infinity. The advertised 120 degrees
  is diagonal FoV, not horizontal FoV.
- MiCode commit `dc1fbbeea69a4cba247387c9654489c4291d75e6`:
  [yupik-camera.dtsi](https://github.com/MiCode/vendor_qcom_proprietary_camera-devicetree/blob/dc1fbbeea69a4cba247387c9654489c4291d75e6/yupik-camera.dtsi)
  provides corresponding CCI1 and CSIPHY3 resources. The candidate translates
  downstream regulator/GDSC references to the actual Q6A regulators/genpd
  and uses the board's 64-bit address cells. It does not copy phone rails.
- A public source mirror at
  [commit 36fc163a534963a5b3af52186af5efcc63401ad2](https://github.com/comprehensive9/vendor_qcom_proprietary/tree/36fc163a534963a5b3af52186af5efcc63401ad2/chi-cdk)
  provides reference sensor API headers, schemas and ParameterParserGCC7.
  These are external vendor reference files, not an SDK-matched development
  package. Their original notices remain intact outside this repository.
  `chi-reference-inputs.json` pins the minimal inputs and SHA-256 values.

## Implemented and checked

1. `fetch-chi-reference.py` fetches or verifies eight pinned reference inputs.
2. The optional sensor-core CHI adapter builds
   `com.qti.sensor.imx708_wide.so` with real external interface declarations.
   It implements exposure calculation and bounded non-HDR register updates,
   rejects unsupported modes/HDR/strobe/long-frame requests, and stays outside
   production installation. Mode index 0 is preview; 1 is full-resolution.
3. `build-reference-module.py` authors Wide module and DW9817 actuator XML,
   validates their structure, and invokes the real compiler to produce bins.
   For supplemental libxml2 validation, two vendor XSD extensions are normalized:
   the complex simple-content list item `RegisterData` becomes `xs:unsignedInt`,
   and `xs:Float` becomes `xs:float`. Original schemas are used by the compiler.
4. `q6a-cam3-camx-candidate.dtsi` contains CCI1/master1, CSIPHY3, sensor,
   actuator, GPIO78 enable and the actual Q6A rail references. It compiles
   together with the real board DTS, without invented phandle stubs. Candidate
   nodes remain disabled, and the file is not selected as a kernel patch.

## Actual compatibility findings

The reference library builds and passes its own callbacks on host and AArch64
under QEMU. However, the same callback harness fails against the SDK's
`com.qti.sensor.cmk_imx577.so.0.1.0`: the old `SensorExposureInfo.lineCount`
is at offset 52, while the SDK binary writes that result at offset 60.
The supplied entrypoint and a matching function name alone do not prove ABI
compatibility. The harness uses extra guard storage and rejects the mismatch.

The reference ParameterParser emits binary header version **2.0.0**. Attempting
to decode the SDK's `com.qti.sensormodule.cmk_imx577_cam3.bin` with this tool
returns **unsupported binary header version 7.0.0**. Thus the reference
module/actuator binaries must not be installed as current Kodiak assets.

The earlier statement that no public reference tools existed was too broad.
Tools and schemas have now been recovered and exercised; the remaining work
is adapting to the current binary format and ABI, plus completing sensor and
ISP platform integration. Board absence only prevents hardware acceptance.

## Reproduce the offline checks

From the repository root (host requires Python lxml, CMake, a C++ compiler,
device-tree-compiler and fdtget):

```sh
python3 apps/imx708-camera/scripts/fetch-chi-reference.py --output /tmp/chi-reference
python3 apps/imx708-camera/scripts/build-reference-module.py \
  --cdk-root /tmp/chi-reference --output /tmp/imx708-module-candidate
cmake -S apps/recipes-camera/imx708-sensor-core/files -B /tmp/imx708-chi \
  -DIMX708_REFERENCE_SOURCE=/path/to/pinned/imx708.c \
  -DIMX708_CHI_CDK=/tmp/chi-reference
cmake --build /tmp/imx708-chi
ctest --test-dir /tmp/imx708-chi --output-on-failure
python3 apps/imx708-camera/scripts/check-candidate-dt.py \
  --kernel-source tmp/work-shared/radxa-dragon-q6a/kernel-source \
  --output /tmp/imx708-dt-candidate \
  --fdtget tmp/sysroots/x86_64/usr/bin/fdtget
```

Use fresh output directories for XML and DT checks. For AArch64, build with
the core recipe's generated CMake toolchain and cross-compiler PATH, then run
`chi-callback-test <library>` under QEMU with the appropriate target sysroot.
The test is a callback/ABI check and does not instantiate CamX or access a sensor.

## Remaining implementation work

- Match current Kodiak callback structures and binary version 7; do not relabel
  a v2 header or assume that changing one field offset completes the port.
- Generate the complete IMX708 sensor XML/register bundle, including conditional
  SPC handling, sensor timing metadata and validated group-hold semantics.
  The current adapter's `0x0104` group hold is a Sony-interface candidate that
  still requires IMX708-specific confirmation before deployment.
- Finish DW9817 busy polling and gradual parking in the CHI path. The generated
  linear DAC region describes electrical limits, not optical focus calibration;
  its direct parking write is only a reference representation. Infinity metadata
  and the shared module enable/VAF power sequence also require target validation.
- Port CRM/CPAS/IFE/CSID/SMMU/ISP nodes and Wide tuning. A compiled disabled
  transport/sensor DT fragment is not the complete hardware-ISP platform.

The selected-image gate remains because these are unfinished software items,
not because a board is absent. No reference asset is automatically deployed.
