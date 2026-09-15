# Q6A XFCE desktop

The project app layer selects XFCE for `qcom-multimedia-proprietary-efi-sd-image`
on `radxa-dragon-q6a`, retaining the image name and OTA ref. Other images keep
their existing desktop configuration.

The `qcom-xfce` package runs Xorg and XFCE through `xserver-nodm.service`, with
PAM autologin as the existing `weston` user. Its home directory and locally
installed XDG application entries are preserved by OSTree. Weston service and
socket activation are masked. The session uses English, disables window
compositing, retains Adreno glamor 2D acceleration, and applies the board's HDMI 8-bpc workaround at login.

`qcom-xfce-ready.service` checks X11 connectivity, the window manager's root
property, and the XFCE session, window manager and panel processes. It also requires
Adreno glamor in the current Xorg log, hardware-accelerated GLX, an Adreno
renderer, and no Xorg GL errors. An additional graphics probe draws an
X11 pixmap, samples it through GLX texture-from-pixmap on the GPU, presents it
to a window, and verifies the actual window pixels. A black window fails the
gate even when the private benchmark FBO renders correctly. llvmpipe/softpipe/swrast fail the gate. The image
adds this service to `/etc/qcom-ota/health.json`; OTA confirmation requires it
to succeed. This is a software readiness gate, not physical HDMI or peripheral
acceptance. Administratively modified health policy must be checked after
OSTree merges `/etc` during installation.

Select the same application as the installed image when building, and set a
strictly newer `QCOM_OTA_VERSION` in a BitBake postconfiguration file:

```sh
source ./environment-setup-armv8a-qcom-linux
layers/oe-core/bitbake/bin/bitbake -R /path/to/selection.conf \
    qcom-multimedia-proprietary-efi-sd-image
```

Follow `README-OTA.md` for signed publication, installation and rollback.
Before reboot, verify the pending image identity, desktop masks, health policy
and retained deployment. After reboot, check the exact commit and confirmed
state, HDMI and keyboard/mouse operation, application launch, and restart
recovery. Keep failed updates unconfirmed and use `qcom-ota rollback` followed
by `qcom-ota reboot` if the desktop fails acceptance.

Run the desktop health regression tests from the SDK root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
    -s layers/meta-radxa-dragon/scripts/tests -p test_qcom_xfce.py -v
```

## Graphics compatibility

Xorg uses native Adreno EGL/GLES for glamor. GTK3's X11 window contexts use
GLX, supplied by Mesa Zink over Adreno Vulkan through session-local
`MESA_LOADER_DRIVER_OVERRIDE=zink` and `GALLIUM_DRIVER=zink`. An explicitly
empty `GDK_GL` selects GTK's desktop GL painting context on X11 and prevents
applications' non-overwriting GLES defaults from selecting the incompatible
shared GLES painting path. These variables
are set in the client session, after Xorg starts. No EGL vendor override or
software rendering fallback is selected. The packaged Adreno Vulkan driver
reports missing Vulkan `logicOp` support to Zink; passing the application
pixel tests does not establish conformance for every OpenGL operation.

Q6A-specific patches address three reproduced failures:

- Glamor imports GBM pixmaps through DMA-BUF when native-pixmap import is
  unavailable. Adreno does not advertise `EGL_KHR_image_pixmap`. Glamor retains
  the DMA-BUF metadata throughout the pixmap lifetime and uses it for DRI3
  re-export, because MSM GBM cannot import an EGLImage. The metadata is
  exchanged and released along with its pixmap image.
- MSM GBM device destruction no longer closes other devices' live buffers.
  Handle references include the DRM fd; the shared mutex is initialized once.
- GLX provider probing invalidates Xorg's cached current context. Temporary
  EGL display teardown can disturb dispatch, so glamor must rebind afterward.

The session logs Xorg under `/run/user/1000/qcom-xfce/`, creates a writable
XDG data directory for the retained user, and activates VT7 before Xorg takes
its logind DRM device. The URM classifier excludes the desktop processes so
they remain in the active seat. Verify locally modified `/etc/urm` policy in
the pending deployment because OSTree preserves administrator edits.

On the board, run the GBM lifetime regression with:

```sh
python3 check-qcom-gbm-lifetime.py
```

The script is in this layer's `scripts/` directory. It tests empty-device
teardown, independent DRM connections and shared imported buffers. Run
`apps/app005-gpu-benchmark` in both offscreen and X11 window modes, retaining
per-mode results and Xorg/kernel logs. `passed` establishes rendering-test
completion; its performance verdict remains `baseline_only`.
