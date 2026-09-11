# IMX708 register reference for Q6A CAM3

The user supplied [6by9's IMX708 driver](https://github.com/6by9/linux/blob/rpi-6.12.y-imx708-4lane/drivers/media/i2c/imx708.c).
Pin the reference to commit
`fac260a153218b4f0897a7974068ff77e2b17b8b`, path
`drivers/media/i2c/imx708.c`, rather than following the moving branch.
The downloaded file SHA-256 is
`d83d25e985faacd8029a7dcc98be9c749e060953b292335337b2e43df9a58704`.
The source carries `GPL-2.0` and Raspberry Pi copyright notices; preserve
those when importing source or register tables. This document records the
porting decisions, without adding that driver to the MIT application.

## Interface and mode selection

Despite the branch name, `imx708_check_hwcfg()` accepts both two and four
CSI-2 data lanes. For the Q6A CAM3/J7 two-lane connection, program the
8-bit register `0x0114` to `0x01`; `0x03` selects four lanes and is unsuitable
for this connector. Receiver lane configuration must agree with the sensor.

The reference assumes a 24 MHz sensor input clock. Its common sequence
programs the 16-bit external-clock register `0x0136` to `0x1800` and RAW10
format registers `0x0112` and `0x0113` to `0x0a`. Confirm the module's clock
source independently of the CAM3 connector's optional MCLK connection.

Non-HDR mode parameters from `supported_modes_10bit_no_hdr`:

| Sensor output | Static register table | Pixel rate | Line length | Default frame length | Calculated default fps |
| --- | --- | --- | --- | --- | --- |
| 4608 x 2592 | `mode_4608x2592_regs` | 595200000 | 15648 | 2650 | 14.35 |
| 2304 x 1296 | `mode_2x2binned_regs` | 585600000 | 7824 | 2494 | 30.01 |
| 1536 x 864 | `mode_2x2binned_720p_regs` | 566400000 | 5216 | 3619 | 30.01 |

Frame length is height plus default vertical blanking; the calculated rate
is pixel rate divided by line length and frame length. These are sensor
timing calculations, not measured Q6A capture rates.

Use 2304 x 1296 as the initial 1080p30 input candidate and scale to
1920 x 1080 in the ISP. There is no native 1920 x 1080 table in this source.
Use the full-resolution remosaic mode for 4608 x 2592 still capture; it is
not a full-resolution 30 fps mode at the stated defaults. Both candidates
still require CamX stream-combination and receiver validation.

## Port the complete startup sequence

The static arrays alone are insufficient. Follow `imx708_start_streaming()`
and its control callbacks when creating the eventual CamX sensor assets:

1. Establish module power/reset timing and validate chip ID `0x0708` from
   the 16-bit register `0x0016`. The reference requires at least 8 ms from
   reset release to capture; I2C ID access has a separate shorter delay.
2. Apply `mode_common_regs` after power initialization. Preserve the
   conditional PDAF gain initialization after reading `0x7b10`; it must not
   become an unconditional replacement of calibration values.
3. Select two lanes and program the mode's pixel PLL multiplier at `0x0306`.
   With the reference divider constants it is
   `pixel_rate * 2 * 5 * 2 / (24000000 * 4)`.
4. Apply the selected mode table, then program 16-bit line length `0x0342`
   and output-link PLL multiplier `0x030e`. The latter is
   `link_frequency * 2 * 2 * 4 / 24000000`; it is independent of the pixel
   PLL. A 450 MHz link gives multiplier 300 (`0x012c`) and 900 Mbit/s per
   lane. This is a bring-up candidate, not a verified Q6A PHY setting.
5. Apply the full-resolution QBC/remosaic settings when appropriate, then
   frame length, exposure, gains, orientation and other controls. Finally
   write `0x0100 = 1` to start; write `0x0100 = 0` to stop.

`CCI_REG16` denotes a 16-bit register value, not an 8-bit write. Preserve
the value width, byte ordering and sequence in any XML conversion.

Exposure uses `0x0202`, with 48 lines reserved below frame length. The
full-resolution mode has minimum 8 lines and step 1; both non-HDR binned
modes have minimum 4 lines and step 2. Analogue gain register `0x0204` has
code range 112..960; these codes are not linear gain multipliers. Digital
gain uses `0x020e`, with unity code `0x0100`. Port the long-exposure shift
handling at `0x3100` if enabling long exposures.

The unflipped Bayer order is RGGB; flips change the order. Embedded data
includes PDAF and AE histogram content. Describe and route it separately
in the CamX/CSI configuration; RAW image reception alone does not establish
PDAF or working autofocus. The sensor driver does not provide Wide-module
lens calibration or a CamX tuning binary.

## Comparison with the current Q6A kernel

The local `linux-qcom` source already contains these three non-HDR output
sizes and pixel rates. Its driver supports two lanes, and explicit
450/447/453 MHz link-frequency tables. The supplied reference moves lane
selection and PLL programming into runtime writes and uses V4L2 CCI helpers.
There is no need to replace the local driver solely to enable two lanes.

The local driver also explicitly programs continuous/noncontinuous clock
behavior during startup. Preserve that receiver/sensor agreement when
porting; replacing it with the supplied file wholesale could lose local
behavior. The Qualcomm CamX sensor ABI, XML compiler, CAM3 proprietary DT
integration and Wide tuning remain separate dependencies.

Validation performed: downloaded the branch and commit-pinned files,
verified identical SHA-256, inspected mode/control/startup code, and compared
local lane/link/mode handling. No sensor writes, flashing or hardware capture
were performed.
