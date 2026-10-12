# OpenHD bring-up status

Evidence date: 2026-10-13 (Asia/Shanghai). `wireless_qualified=false`.

All implementation and environment extensions now live in this app directory,
including the private `meta-openhd` layer. Shared AIC recipes and
`apps/conf/layer.conf` were restored. The only shared edit made by this task
is the app registration in `apps/README.md`; pre-existing app003 edits remain.
The BitBake isolation check passed: including the private driver append changed
no driver task base hashes for no app, app003, or app012. The app013 selection
alone enabled the experimental build flag, patches and build-marker install.

## Commit checkpoint (2026-10-13)

- P0 source/build baseline is complete: pinned OpenHD/sysutils, the raw-frame
  probe, board inventory, temporary monitor/driver runners and local hardware
  video preflight are available in this app. The private layer's isolation
  checks passed for the ordinary driver and app003/app012 selections.
- P1 remains unqualified. The latest diagnostic driver trial completed
  105/105 local sends without socket errors, then restored the original
  driver and managed Wi-Fi. This does not establish RF reception, actual
  MCS/HT20/NOACK behaviour or sustained wireless delivery.
- Local test-source and UVC trials each decoded 300 H.264 frames with the
  expected GOP, progressive 30 fps VUI and matching buffer PTS. Native
  OpenHD/FEC, managed video and all P2-P4 bench gates remain pending.
- The current source input digest is
  `09c39739b9e7c45844f5a5fafb6a73a8c520296d635ac96629d7b303811ebcd3`,
  packaged as development version `1:0.1.0-r0`. The sealed v3 bundle and
  build/board evidence remain outside Git under `/data/q6a-openhd/`.
- Commit validation passed: `scripts/qcom-app validate --app app013-openhd
  --machine radxa-dragon-q6a`, the current host build and
  `ctest --test-dir /data/q6a-openhd/host --output-on-failure` (7/7).
  This status-only update does not change the packaged source input digest.
- The Codex goal remains **blocked** because no independent supported
  receiver is available. Resume with receiver hardware, its monitor
  interface and SSH/local access; complete P1 before capability-profile or
  native video integration. `wireless_qualified=false` remains unchanged.
- This checkpoint is for a local source commit scoped to `app013-openhd`
  and its `apps/README.md` registration. It preserves the two existing
  app003 document edits and 752 buildtools deletions; no push or board image
  deployment is part of this checkpoint.

## Verified

- Q6A identity and changed SSH host fingerprint verified using `/dev/ttyACM0`.
  Ethernet management uses `192.168.2.5`; task-specific SSH trust is stored
  outside the checkout at `/data/q6a-openhd/known_hosts`.
- Onboard AIC8800D80 is `a69c:8d81`. Current installed kernel is
  `7.0.11-6-qcom`; its original Wi-Fi module lacks the monitor TX build marker.
- Read-only board inventory completed. Original Wi-Fi module and firmware
  archive: `/data/q6a-openhd/reports/board-wifi-original.tar.gz`, SHA-256
  `c1ec6882aac640e57e327587c3b808370ba67ba045a8f8abe1490a692f275fd1`.
- App manifest validation and host CTest passed (7/7). The separately
  cross-compiled raw-frame probe passed its protocol self-test on Q6A and
  rejected TX on the managed interface without sending frames.
- Independent kas metadata parsing passed. Existing eSDK buildtools deletions
  were preserved. Target tools/driver build and package QA passed
  (4,201 tasks, `tools-build-v5.log`). The updated app subsequently passed
  all 4,201 aggregate build tasks and package QA (`tools-build-v6.log`).
  The initial tools build caught unshipped JSON configuration files; their
  install path is now within the selected app's packaged data directory.
- Upstream sysutils built on the host. A second build retained the generated
  version-header SHA-256 `b478cdbda79467d313cd2b7798bcb71a4b93d4debb681e40364bb50fd727fba0`.
- All seven private patches passed the OpenEmbedded patch metadata validator.
  This scoped check is distinct from the full layer CI wrappers/selftest.
- The experimental driver build completed all 1,101 tasks, including compile,
  package QA and RPM generation (`driver-build-v3.log`). OpenHD/sysutils use
  actual pinned Git commits, including all four core submodules. Private
  patches handle GCC 15's explicit integer header requirement and parsing the
  sysutils native compiler command. Both target executables completed build,
  package QA and RPM generation (`upstream-build-v4.log`, 3,311 tasks).
- The actual Yocto probe passed its self-test on Q6A, and target sysutils
  `--version` reported `1.0.3.dca9b1baa7c4`. Core shared-library resolution
  passed using temporary private Poco/libsodium/utf8proc libraries in
  `/var/tmp`. The core/sysutils daemons were not started or installed.
- The P0 development artifact bundle is
  `/data/q6a-openhd/artifacts/p0-2026-10-13/`, with executable/RPM/module,
  patch, build-log and provenance hashes in `manifest.json`. This preserved
  bundle's app input digest is
  `1804bf28780c03580d948edd0bba52f236ee4e9d5aff6005f4bac5b0918fc042`.
  This is a development bundle, not a qualified boot image or transmitter.
  The subsequent host bitstream analyzer and preflight report metadata change
  have input digest
  `ab85341f3420d57e920618f3dbbde2200a40c528884447d308f5b05a3c792976`;
  their target package build and QA passed. Its versioned development bundle
  is `/data/q6a-openhd/artifacts/p0-2026-10-13-v2/` (67 file hashes plus
  `manifest.json` checked using `SHA256SUMS`). The archive is
  `/data/q6a-openhd/artifacts/p0-2026-10-13-v2.tar.gz`, SHA-256
  `6d97b529d2df178356e1792030587690279697a41047ec8cb8755d8a788782dd`.
  The manifest explicitly identifies reused pinned upstream/driver artifacts
  and includes both original and recovery video evidence. Rebuilding the
  experimental driver produced the same module hash as the tested module.
  The v6 target probe also passed its protocol self-test on Q6A; the final
  check confirmed that Ethernet and managed Wi-Fi remained connected
  (`/data/q6a-openhd/reports/board-probe-selftest-v6.log`).

## Receiver evidence preparation

The RX probe now accepts optional `--capture FILE.pcap`, preserving original
Radiotap/802.11 bytes for valid frames with the selected run ID, including
duplicates. Local outgoing traffic and truncated input are excluded. A 256 MiB
cap and write/sync failures stop the trial with a nonzero exit status; packet
socket receive/drop counters are reported separately from matching packets.
PCAP timestamps are receiver userspace realtime, not synchronized latency.
The capture is filtered diagnostic data: a separate full monitor trace is
needed for ACK/retry behaviour. Missing receiver MCS/BW metadata remains an
unverified gate; requested TX fields cannot fill it in.

Host tests cover byte preservation, unaligned record boundaries, timestamps,
exclusive evidence creation, size limits and invalid timestamp rejection.
An independent tcpdump reader successfully decoded the generated two-record
fixture as Radiotap PCAP (`pcap-synthetic-tcpdump-v7.log`). This is synthetic
format verification, not received RF evidence. Live receiver capture is pending.

The first updated target build compiled successfully but failed package QA:
the shared app recipe's content-hash version sorted lower than the preceding
hash (`tools-build-v7.log`, `version-going-backwards`). The app's private append
now defines development version `1:0.1.0-r0`, retains the input digest in a
packaged `build.json`, and requires PV/PR increments for future releases.
No package QA checks were disabled. `app-and-driver-isolation-v8.log` passed:
the app version extension changes no selected-app task hashes or version
variables for app003/app012; the driver extension still changes no driver
task hashes for no app, app003 or app012. The corresponding target rebuild
passed all 4,201 tasks, including package QA/RPM (`tools-build-v8.log`).
The target probe passed its Q6A protocol self-test, and RX/capture on the
managed interface was rejected before either evidence file was created;
Ethernet and managed Wi-Fi remained connected (`board-probe-selftest-v8.log`).

Current input digest:
`09c39739b9e7c45844f5a5fafb6a73a8c520296d635ac96629d7b303811ebcd3`.
The new development bundle is `/data/q6a-openhd/artifacts/p0-2026-10-13-v3/`;
all 74 file hashes plus its manifest passed `SHA256SUMS`. Its archive is
`/data/q6a-openhd/artifacts/p0-2026-10-13-v3.tar.gz`, SHA-256
`c86a1b430cfa995788724e421b9586d2423f9ecbf03f4722f18b0d0f4cd9c74b`.
Earlier bundles remain unchanged. The new manifest marks PCAP format evidence
as synthetic and live receiver capture as untested.

Read-only receiver discovery found no registered WLAN PHY or PCI wireless
controller on the host (only its Realtek Ethernet controller), and board
inventory still showed one onboard AIC radio. No independent receiver is
currently available. Evidence: `host-receiver-discovery-v8.json` and board
inventory; the physical RF gates below remain incomplete.

## Temporary driver trials and recovery

An initial monitor trial hit a reproducible driver NULL access in
`rwnx_rxdataind_aicwf`: the firmware supplied MONITOR|FORWARD (0x81) while
`CONFIG_RWNX_MON_DATA` was disabled, leaving `skb_monitor` NULL. The RX thread
and subsequent unload failed. Software reboot then stalled in the AIC reboot
callback; the user power-cycled the board after filesystem sync. The original
disk module and firmware were never replaced.

The private third driver patch handles the mixed flags, missing monitor vif
and allocation failure. Its compiled disassembly no longer contains the
observed constant-NULL path. The fourth patch selects the running monitor
netdev queue. Current experimental module SHA-256:
`133ed5883886a15ef2706ebb8598464a4228d6707b01b558f6f20850f78e4ca9`.
The earlier module ending in `...bece3b` is unsafe and must not be reused.

Trial 04 normal-loaded the fixed module, switched to 5180 MHz/HT20 and ran
one second of diagnostic raw-socket TX. It accepted 93 of 105 attempted
packets (0.886 Mbps payload) with 12 socket errors. This is a local socket
observation with no independent receiver; it does not prove transmission,
actual MCS/NOACK or usable RF throughput. No new kernel Oops/refcount warning
was found in the trial's kernel-log delta. The original module, parameters
and saved Wi-Fi connection were restored automatically.

Traced trials 05/06 stopped before TX because setting 5180 MHz/HT20 returned
EBUSY. The trace showed that the interface had reverted to managed after the
successful type-change command. NM's unmanaged state alone did not guarantee
that the supplicant had completed teardown. The final app runner preserves
and temporarily unmanages only the same radio's idle P2P companion, waits for
the original supplicant D-Bus interface to disappear, and reads back monitor
mode and the requested channel before starting TX. Active P2P use is rejected
before any network changes; management policy is restored afterwards.

Tracing confirmed all 12 send errors were EAGAIN, after the first 93 packets
had filled the socket buffer. Source inspection found that monitor open
starts netdev queue zero, but the selector returned the stopped BCMC queue.
The fourth private driver patch selects queue zero only for experimental
monitor TX and passed target build/package QA. The updated probe
records errno counts and returns `TX_SOCKET_ERRORS`/nonzero on any send error.
Trial 08 verified this behavior and restored the original module/connection.
These are diagnostic failures, not RF throughput results.

Trial 12 traced the patched queue and Q6A's actual sysfs/ioctl monitor type
(803); the Yocto-built probe accepted 105/105 sends with zero socket errors.
Trial 13 used the final app runner without external trace delays and again
accepted 105/105 sends (approximately 1 Mbps payload for one second), with
zero socket errors. Both normal-loaded the module and restored the original
module, saved connection and P2P management policy. This remains
`LOCAL_TX_DIAGNOSTIC_ONLY`, not independent over-the-air observation.

The recovered module SHA-256 remains
`5186e683aac0676f803305bafcba599500446ac0ce72caf9f16037a4376ac523`;
Ethernet `192.168.2.5` and managed Wi-Fi `192.168.2.6` are connected. Reports:
`/data/q6a-openhd/reports/openhd-driver-trial-13/`,
`board-driver-trial-13-inspect.log`, and `board-recovery-verification.log`.

Kernel release, enabled configuration, compiler and build timestamp match
the SDK kernel build, and normal module loading worked without any force
option. The board and local kernel GNU build IDs differ; this is not proof
of exact kernel-binary or full ABI equivalence.

## Local video observations

Both the test pattern and attached MJPEG 1080p30 UVC camera produced hardware
H.264 output. No software H.264 encoder was used. Initial 300-frame streams
had zero B frames. A GOP control value of 14 produced keyframes every 15 frames.

An additional instrumented trial read the controls from the encoder's active
file descriptor: CBR, 6 Mbps, no B frames, GOP 14, repeated SPS/PPS,
`time_delta_based_rc=0`, and low-latency mode all matched the requested values.
Both V4L2 queue frame-rate readbacks were 30 fps.

Both instrumented trials recorded 300 capture, encoder-input and
encoder-output buffers. Test-pattern PTS rate was 30.000 fps and UVC PTS rate
30.044 fps; largest source PTS gaps were 33.333 ms and 44.241 ms respectively.
The UVC coded payload was approximately 5.60 Mbps. These short observations
do not establish sustained frame delivery.

Complete host decoding of each saved trial produced 300 progressive I/P
frames, no B or repeated pictures, and keyframes at indices 0, 15, ..., 285.
All capture/input/output PTS values corresponded exactly, and every IDR
contained preceding in-band SPS/PPS. Each stream contained 21 traced SPS
headers (including initial extradata), with fixed progressive timing:
`num_units_in_tick=2097152`, `time_scale=125829120`,
`frame_mbs_only_flag=1`, `fixed_frame_rate_flag=1`,
`pic_struct_present_flag=0`. This is a 60 Hz tick clock and 30 fps,
`time_scale / (2 * num_units_in_tick)`. The earlier interpretation of raw
`ffprobe`'s `60/1` rate estimate as a 60 fps VUI mismatch was incorrect;
no encoder setting or bitstream rewrite is required by this observation.
The raw elementary-stream estimates (`r_frame_rate=60/1`,
`avg_frame_rate=25/1`) are preserved separately from measured source PTS.
Analysis reports and full header/decode logs are
`/data/q6a-openhd/reports/openhd-test-analysis-v6/` and
`/data/q6a-openhd/reports/openhd-uvc-analysis-v6/`.

These approximately ten-second trials do not establish displayed fps,
OpenHD-managed video, wireless throughput, end-to-end latency, or stability.
Reports and streams are under `/data/q6a-openhd/reports/`; instrumented trial
reports are `openhd-test-v5/report.json` and `openhd-uvc-v5/report.json`.

The updated packaged preflight script was also run after recovery of the
original driver. Fresh `openhd-test-v6` and `openhd-uvc-v6` trials passed the
same complete 300-frame decode, GOP/VUI and PTS correspondence checks.
Test-source PTS rate remained 30.000 fps; UVC was 30.021 fps, with a largest
source gap of 40.423 ms and approximately 5.76 Mbps coded payload. Their
analysis evidence is in `openhd-test-recovery-analysis-v6/` and
`openhd-uvc-recovery-analysis-v6/`. These retain the same local-only boundary.

## Remaining gates

1. An independent supported monitor/OpenHD receiver is required for raw RF
   delivery, actual MCS/HT20/NOACK checks and the 30-minute FEC payload test.
2. After RF qualification, add the Q6A-specific capability profile and managed
   hardware video path. Carry the verified controls/frame accounting and
   VUI checks into that pipeline, verify sustained capture timing, and measure
   the actual steady-state bitrate.
3. Complete both 30-minute source tests, optical latency sampling, recovery,
   two-hour soak and ten cold boots before image/service qualification.

No QSPI flashing, transmitter autostart or generic AIC support whitelist has
been performed. The Codex goal is blocked on the unavailable independent
receiver after three consecutive checks with the same physical blocker and
no further independent implementation or acceptance work available. The full
objective remains unfinished. Resume with a supported receiver's interface
and SSH/local access details; RF, native FEC, managed video and bench gates
must pass before marking the goal complete.
