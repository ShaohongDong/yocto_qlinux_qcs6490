# Native CAM3 preview and RAW diagnostic

The selected app image integrates the production native device tree and starts
a maximized preview through `imx708-native-preview.service` after Weston and
`imx708-native-drivers.service`. No diagnostic BLS entry is needed for normal
use. The procedures below remain available for isolated driver diagnosis.

`imx708-camera-launch` is shared by the service and desktop entry. It runs as
the Weston session user, waits at most 30 seconds for devices, and uses
`--backend=native --native-autostart --maximized`. One native instance per user
is allowed. Normal close does not trigger a service restart. Photos from this
launcher go to `~/Pictures/imx708-camera`. Native CLI also accepts `--maximized`.

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

## Native application preview

Build with `-DIMX708_WITH_QMMF=OFF` for an independent native-only executable,
or leave QMMF enabled to retain both backends. The default CLI backend remains
CamX. `--test-source` remains synthetic and cannot be combined with native mode.
The native backend requires `media-ctl` at runtime; it configures the existing
media graph but does not install DTBs, load modules or change boot entries.

After staged driver loading, run inside the board's Weston session:

```sh
imx708-camera --backend=native --native-autostart --output-dir /var/tmp/photos
```

For temporary SSH diagnostics, use the existing session's `XDG_RUNTIME_DIR`,
`WAYLAND_DISPLAY` and appropriate device access. Keep the executable and output
in a separate temporary directory; do not replace the installed application.

The native UI receives 2304x1296 RAW10 at approximately 30 fps and displays
1152x648 RGB, scaled proportionally to fit the screen. Exposure is expressed in
sensor lines, and analogue gain in driver register codes; ranges come from the
driver. The Apply button requests new settings on the capture thread. Three
frames are discarded after sensor exposure/gain changes to allow settling.
Software colour settings are black level (default 64), red/blue multipliers
(default 1), and fixed gamma 2.2. These defaults are not calibrated ISP tuning.

Photos use the first valid frame whose monotonic EOF timestamp follows the
request. A background task writes matching `.jpg`, `.raw` and `.json` files;
JSON records format, stride, sequence, timestamp and applied settings. JPEG is
1152x648; RAW remains 2304x1296 packed RGGB10. All files use unique names and
`.partial` staging. The JPEG is published only after its companions exist.
Failures retain incomplete files and never report a successful photo.

Capture owns four MMAP buffers and requeues promptly. The appsrc preview queue
holds at most two RGB buffers and drops the oldest on overload. A bad/short
buffer is rejected, sequence gaps are counted, and three seconds without a
valid frame stops the stream. Stop, window close and SIGINT/SIGTERM perform
STREAMOFF, unmap buffers and join the capture thread. Photo encoding/writing
does not block capture. Native recording, AE, AWB and autofocus are unavailable.

Before STREAMON, the worker waits for the sensor's normal runtime autosuspend
to finish (up to seven seconds, cancellable in 100 ms intervals). This ensures
a complete driver-managed power cycle, including after a cancelled start or
another process's colour-bar session. Warm colour-bar-to-image transitions on
this module otherwise can produce no frames. Already-suspended sensors start
immediately; rapid stop/start may wait about five seconds. No GPIO or PM policy
is changed by the application.

Diagnostic CLI options:

- `--native-test-pattern`: show sensor colour bars; photos are disabled.
- `--native-exercise-seconds N`: real camera only, perform five restarts and
  three photos (baseline, increased gain, reduced exposure) through the GTK
  controls and Apply path, restoring original settings afterwards. Stop after
  N seconds from the first displayed frame in the final streaming session
  (10–3600). This is a physical hardware exercise, unlike `--exercise-dir`.

On the analysis host, `scripts/check-native-photos.py PHOTO_DIRECTORY` checks
JPEG/RAW/JSON grouping and compares JPEG pixels with an independent numpy RAW
rendering reference, allowing for JPEG compression. It does not grade image
quality. Keep the scene stationary for the exposure/gain comparison.

Check per-second `capture_fps`, `display_fps`, `bad` and `gaps` logs along with
physical screen observations. Display counts mean delivery to the GTK sink;
they do not prove panel refresh or colour accuracy. After testing, follow the
diagnostic entry archival and normal-boot restoration procedure above.
