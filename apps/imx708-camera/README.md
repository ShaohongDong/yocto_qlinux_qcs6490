# IMX708 Camera for Q6A CAM3

Target: Raspberry Pi Camera Module 3 Wide on Q6A CAM3, with the HDMI Weston
session. Selecting this app enables the Linux IMX708 CCI1/master1/CAMSS path and
opens a maximized native preview at boot. The selected kernel includes the CCI completion initialization fix.

Native capture is 2304x1296 RAW10 at approximately 30 fps. The preview and JPEG
are 1152x648 RGB with manual exposure, analogue gain, black level and red/blue
multipliers. Software colour processing is not calibrated ISP tuning. Native
recording, AE, AWB and autofocus are unavailable. See [native usage](docs/native-raw.md).

## Build

From the SDK root in a fresh shell:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app imx708-camera --machine radxa-dragon-q6a
scripts/qcom-app image --app imx708-camera --machine radxa-dragon-q6a
```

The default image is `qcom-multimedia-proprietary-efi-sd-image`. The native image
checks the selected device tree, modules, dependencies and startup services;
`--allow-incomplete-camera` is not required for this path. The UFS image remains
an allowed build target but is not covered by SD/OTA acceptance.

## Use

At boot, the `weston` user's preview starts automatically and maximizes without
hiding the desktop panel or window controls. Closing the window releases the
camera and does not reopen it automatically. Launch **IMX708 Camera** from the
desktop to reopen it; repeated launches do not create competing native streams.
The startup service and desktop entry use `imx708-camera-launch`, which waits up
to 30 seconds for the Weston session and camera devices. Missing camera devices
leave a retryable error in the window. A missing display is reported in the
service journal.

Photos are saved as matching JPEG/RAW/JSON groups under
`/home/weston/Pictures/imx708-camera`. Image colours are uncalibrated; adjust
exposure/gain and software colour controls manually. When launching the binary
directly, use `--backend=native --native-autostart --maximized`; omit
`--maximized` for a normal resizable window, or set `--output-dir DIR`.

The optional `--backend=camx` backend and the binary's legacy default remain
available for development. CamX sensor, actuator, tuning and board integration
are incomplete; native enablement does not validate that path. See
[CamX bring-up](docs/bringup.md), [register core](docs/sensor-core.md) and
[material compatibility](docs/materials-progress.md).

## Offline tests

Host dependencies: GTK3, GStreamer core/app development packages; runtime
`gtksink`, `videotestsrc`, `jpegenc`, `x264enc`, `h264parse`, `mp4mux`, Xvfb and
ffprobe. Build without Qualcomm libraries only for host tests:

```sh
cmake -S apps/imx708-camera -B /tmp/imx708-host -DIMX708_WITH_QMMF=OFF
cmake --build /tmp/imx708-host -j4
ctest --test-dir /tmp/imx708-host --output-on-failure
xvfb-run -a /tmp/imx708-host/imx708-camera --exercise-dir /tmp/imx708-media
python3 apps/imx708-camera/scripts/check-media.py /tmp/imx708-media
python3 apps/imx708-camera/scripts/audit-sdk.py --sdk .
```

`--self-test` needs neither a display nor a camera and is suitable for the
framework's QEMU rootfs check. `--test-source` explicitly uses synthetic frames
and a software encoder; it is never chosen automatically. `--exercise-dir`
tests full-size JPEG, repeated recording and closing while recording. These
tests do not verify autofocus, ISP quality, hardware encoding or board fps.
Add `--exercise-failure` to inject a camera-source error while recording;
the expected result is exit status 1 with an unpublished `.mp4.partial`.
