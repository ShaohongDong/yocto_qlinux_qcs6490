# CAM3 platform binding audit

This is a source audit, not an enabled device tree or board validation.
The subsequent [material-based implementation](materials-progress.md) adds an
inactive CCI/sensor/actuator/CSIPHY candidate and verifies it against the real
Q6A board DTS; the full ISP platform remains to be ported.
Kernel baseline: `linux-qcom` commit
`657c0f722940cd9d3b51abfa7383655ec7d2c795`; CamX driver baseline:
`0f16924ff6a7f9bb56a7e958016da2ed8a174f2f`.

## Confirmed board and source mapping

Q6A schematic V1.21 CAM3/J7 routes CSI3 and CCI_I2C3. The SoC sheet maps
CCI_I2C3 SDA/SCL to GPIO75/GPIO76 and CAM3_RESET_N to GPIO78. The kernel's
`arch/arm64/boot/dts/qcom/kodiak.dtsi` has matching `cci3_default` pins.
Its native controller `cci1` at `0x0ac4b000` contains master 1
(`cci1_i2c1`). This establishes the source mapping, not the physical FPC
orientation or the camera-service enumeration ID.

The intended CamX sensor mapping is therefore CCI controller 1, master 1,
CSIPHY index 3. Module slot index must be assigned consistently with the
eventual CHI module configuration; it is not inferred from connector number.
Verify reset polarity against module enable/XCLR semantics before generating
the final power sequence. The schematic does not establish the lens actuator
identity or its register protocol.

## Current platform gap

The Q6A DTS includes `kodiak.dtsi`, which provides native
`qcom,sc7280-cci` controllers and `qcom,sc7280-camss`. It does not provide
the complete proprietary camera platform used by `camx-dlkm`. Adding just
a `qcom,cam-sensor` node does not create CPAS, CRM, IFE, CSID, CSIPHY,
SMMU, JPEG and other required CamX devices/resources.

The inspected camera-driver source contains no camera platform DTS/DTSI
files. A compatible platform device-tree source must be obtained or the
required resources ported and validated against this driver before adding
the final sensor node. Do not disable native resources until their CamX
replacements have been integrated.

## Properties verified against the actual driver

In `camera_kt/drivers/cam_sensor_module/cam_sensor/cam_sensor_soc.c`:

| Property | Interpretation |
| --- | --- |
| `cci-master` | Sensor's CCI master; missing property silently defaults to 0 |
| Parent `cell-index` | CCI controller index; missing property defaults to 0 |
| `csiphy-sd-index` | CSI PHY subdevice index |
| `actuator-src`, `eeprom-src` | Phandles to associated subdevices |
| `sensor-position-pitch/roll/yaw` | Orientation; unknown defaults to 360 |

The sensor compatible in `cam_sensor_dev.c` is `qcom,cam-sensor`.
GPIO and regulator parsing also runs through the common `cam_soc_util`
and `cam_sensor_util` helpers. A legacy `qcom,cci-master` property does
not satisfy this driver's `cci-master` read. Check every property against
the selected driver instead of copying an Android BSP fragment.

The native `kodiak.dtsi` alias `i2c19` currently repeats `cci1_i2c0`, whereas
the CCI3 pin group belongs to master 1. Do not use that alias as evidence
for CAM3, or use an unverified Linux I2C bus number for direct register writes.

## Remaining acceptance

Obtain/port the compatible CamX platform DT; establish module slot and power
sequence; compile the DTB and validate resource ownership, interrupt/SMMU
configuration, CCI transactions, chip ID, receiver errors and runtime ISP
capture. Kernel/module compilation alone does not close this platform gap.
