# IMX708 Camera for Q6A CAM3

Target: Raspberry Pi Camera Module 3 Wide (normal IR-cut version), Q6A CAM3,
HDMI graphical session, Qualcomm CamX hardware ISP. The C++/GTK application
uses the actual `qtiqmmfsrc` camera-service backend. **IMX708 CamX sensor,
actuator, tuning and board DT integration are not yet available. This is not
a working board camera release.** See [bring-up evidence](docs/bringup.md).
The [source-built register core](docs/sensor-core.md) now supplies ordered
register tables, timing and exposure/gain calculations with host tests.
Reference CHI callbacks, module/actuator binary generation and an inactive
CAM3 DT candidate are now implemented; see [material-based progress](docs/materials-progress.md)
for the measured ABI and binary-format differences that still prevent deployment.

## Build

From the SDK root in a fresh shell:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app imx708-camera --machine radxa-dragon-q6a
scripts/qcom-app build --app imx708-camera --machine radxa-dragon-q6a
```

`image` selects `qcom-multimedia-proprietary-efi-sd-image`; UFS 4K is also
explicitly allowed by the manifest. Both image builds currently fail at
`do_imx708_support_check` with the missing integration requirements. Client
and register-core builds remain available. The existing kernel contains `imx708.ko` and CAMSS;
neither alone connects an IMX708 to the proprietary ISP. No guessed kernel
patches, device tree, sensor binaries or copied IMX577 tuning are installed.

## Use

Launch **IMX708 Camera** from the HDMI desktop or run `imx708-camera` in the
Wayland/X11 session. Start preview, choose automatic controls or manual
exposure/ISO and white-balance presets, take photos or start/stop recording.
Manual color temperature is available through the white-balance selector.
Single autofocus sends an AF trigger; continuous focus is selectable.

The camera-service enumeration is queried at runtime. Auto-selection requires
an IMX708 sensor-name vendor tag. If the HAL omits that tag, identify the CAM3
sensor from board logs, then launch `imx708-camera --camera-id ID`. **CAM3 is
not necessarily camera ID 3.** Explicit ID selection is a board binding, not
independent proof of sensor identity.

Preview and video request 1920x1080 at 30 fps. Still capture requires CamX to
advertise 4608x2592 JPEG. Recording uses `v4l2h264enc`, H.264 parsing and MP4
muxing, with no software encoder fallback in normal mode. Preview may pause
when entering/leaving recording. Still capture is disabled during recording.
Files use the user's Pictures/Videos directories, unique names and mode 0600.
They retain `.partial` until completion; recording stop and window close wait
for EOS before publishing MP4. Errors leave incomplete files for diagnosis.
The UI is started manually. Camera-service retains its recipe's systemd
startup policy. The image dependency includes a Chinese font for the UI.

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
