# ISP prerequisites and isolated CCI diagnostics

Native RAW capture and software preview are the recovery baseline. A successful
native CCI transaction does not validate Qualcomm CamX, CHI, tuning or ISP output.
The completion patch is selected by `app.yaml` for the native CCI path. The
offline diagnostic tools do not install modules or alter boot entries. Recovery
uses the retained GPIO-I2C 2.0.4 deployment, not runtime bus switching.

## Completion initialization defect

In the SDK's `i2c-qcom-cci.c`, probe initializes each master's completion only
while visiting enabled I2C child nodes. However, controller reset always waits
on master 0's completion, and the reset-done IRQ completes master 0. With only
master 1 enabled, master 0's wait queue is uninitialized. `reinit_completion()`
resets the done counter; it does not initialize that queue.

`kernel/diagnostics/0001-cci-initialize-all-completions.patch` initializes every
supported master's completion before parsing children and requesting the IRQ.
The host harness executes the actual probe preparation and reset functions with
instrumented stubs. It detects the original master-1-only defect and passes for
the patched driver. It does not simulate MMIO or prove interrupt timing.

The previous lockup's first exception was not captured, so the source defect
alone cannot prove every symptom of that incident. Successful patched controller
probe and sensor transactions establish the corrected path's limited acceptance.
Module unload is outside this test: the driver's remove path also halts disabled
masters and needs a separate runtime-PM review. Restore by reboot, not `rmmod`.

## Compatibility and platform boundaries

| Component | Local evidence | Required before ISP integration |
| --- | --- | --- |
| CHI sensor ABI | Reference callback harness passes its own library but rejects SDK IMX577 exposure `lineCount` layout | Headers matching the installed Kodiak CHI/CamX binaries; validate the entire callback contract |
| Module compiler/schema | Reference compiler emits header v2; SDK module bins use v7, which the reference decoder rejects | Matching v7 compiler and schemas; do not relabel a v2 binary |
| IMX708 assets | SDK `chicdk-kodiak` 1.0.25 package has no IMX708 asset; public reference adapter remains development-only | Complete sensor mode/register metadata, Wide module configuration, actuator behavior and tuning |
| Native CCI | `qcom,sc7280-cci`, Linux I2C adapter, CCI1/master1 | Completion fix plus board probe/read/PM evidence; this is not CamX CCI |
| CamX transport | Disabled candidate provides `qcom,cci`, `qcom,csiphy`, sensor and actuator nodes | Validate downstream driver bindings and power/clock/interrupt sequencing |
| CRM/CPAS/IFE/CSID/ISP | Normal Q6A DT has native `qcom,sc7280-camss`; candidate is only transport/sensor | Port the complete matching CamX resource graph; native CAMSS cannot substitute for it |
| Camera SMMU | Normal DT has generic ARM/QCOM SMMU resources | CamX context-bank bindings, stream IDs and DMA mappings require a separate port |

The inspected camera-driver revision is
`0f16924ff6a7f9bb56a7e958016da2ed8a174f2f`. Its `camera/drivers/` bindings include
`qcom,cam-req-mgr`, `qcom,cam-isp`, `qcom,cci` and `qcom,csiphy`; neither normal DT
nor the disabled candidate supplies the complete platform. A successful candidate
DT compilation only checks references and that candidate nodes remain disabled.

The local SDK archives, existing downloads and `/home/dsh/Desktop/q6a-dev` did
not yield a matching sensor API/compiler package. Public reference materials
are catalogued in [materials-progress.md](materials-progress.md). Vendor package
acquisition can start from [QCS6490 software resources](https://www.qualcomm.com/internet-of-things/products/q6-series/qcs6490/software)
and the [official CamX driver packaging repository](https://github.com/qualcomm-linux/pkg-camx-dkms).
These links do not establish availability of a particular CHI development kit.

## Offline preparation

Use a fresh output directory and the exact source/build artifacts corresponding
to the running kernel. Copy the driver to an external-module directory and apply
the diagnostic patch to that copy. Preserve the normal source and installed
module. Build with the SDK cross compiler and existing kernel build artifacts,
then compare `modinfo -F vermagic` with the installed module.

```sh
python3 apps/imx708-camera/scripts/check-cci-completions.py \
  tmp/work-shared/radxa-dragon-q6a/kernel-source/drivers/i2c/busses/i2c-qcom-cci.c
# Expected failure for the unpatched master-1-only case.
python3 apps/imx708-camera/scripts/check-cci-completions.py \
  /path/to/patched/i2c-qcom-cci.c
python3 -m unittest discover -s apps/imx708-camera/tests -p test_cci_dtb.py
python3 apps/imx708-camera/scripts/prepare-cci-dtb.py \
  --base /path/to/backed-up-production.dtb --output /path/to/new/cci.dtb
```

The DT generator disables native CAMSS, GPIO I2C, both automatic sensor paths,
CCI0 and CCI1/master0. Only CCI1/master1 is enabled, at 100 kHz, with CAM3 pins
GPIO75/76 and alias `i2c19` pointing to master1. It inherits the baseline's CCI
clocks and power domain; it does not synthesize new MMIO resources.

## Controlled board procedure

1. Record normal BLS files, DTB, boot configuration, deployment identity and
   module checksums. Keep continuous serial capture from before reboot. Confirm
   an operator can power-cycle the board if SSH and the kernel stop responding.
2. Place diagnostic files under a separate persistent boot directory. Never
   overwrite OSTree kernel/DTB files or installed `/usr/lib/modules` files.
3. First create a control BLS entry using the original DTB. With the current
   embloader `default ostree-*`, the name must match that glob, for example
   `ostree-isp-preflight-control+1.conf`, with `sort-key 00-q6a-isp-preflight`.
   Copy the active deployment's root, kernel and initrd options exactly.
4. Add a diagnostic cmdline marker, blacklist `imx708,qcom_camss,i2c_qcom_cci,i2c_gpio`,
   and mask `imx708-native-drivers.service`, `imx708-native-preview.service`,
   `qcom-ota-confirm.service` and `systemd-bless-boot.service`. Confirm the control
   marker and consumed `+0-1` filename after boot. Reboot again without changing
   the entry to prove exhausted-entry fallback selects the normal deployment.
5. Archive only the owned exhausted entry outside `loader/entries`, then rerun
   OTA confirmation. A counted extra entry referring to the same deployment can
   make confirmation ambiguous even when the normal entry booted successfully.
6. Arm `ostree-isp-preflight-cci+1.conf` with the same isolation and separate CCI
   DTB. Verify its marker, consumed count, module absence and actual DT nodes
   before manually loading the patched module. Record return code and kernel log.
7. If probe succeeds, identify the adapter by its OF path, request TLMM GPIO78
   high, and issue only one combined write `[0x00, 0x16]` / two-byte read at
   seven-bit address `0x1a`. Expected ID is `0x0708`. On success, perform two more
   reads separated by at least three seconds, recording runtime PM state before
   and after. Stop on the first failure. No broad bus scan, autofocus writes,
   sensor streaming or module unload belongs in this diagnostic.
8. Reboot normally, or power-cycle on a kernel fault after preserving serial
   evidence. The exhausted entry must fall back. If it does not, power off and
   remove only the diagnostic BLS entry through the SD reader; retain normal
   files and system data. Do not retry a failed CCI test without new evidence.
9. Archive the exhausted CCI entry, confirm normal boot files are unchanged,
   restart OTA confirmation, and verify 2.0.4 preview/HDMI/SSH recovery. Release
   the serial capture. Keep logs and per-run acceptance results in `artifacts/`.
