# Q6A OpenHD development application

This application implements the staged onboard AIC8800D80 OpenHD bring-up.
It targets one H.264 1080p30 video stream at 6 Mbps, using a Q6A transmitter
and an independent supported OpenHD ground station. The glass-to-glass target
is P95 <=150 ms. No flight-control MAVLink, RC or outdoor-range acceptance is
included in the first version.

## Current acceptance boundary

The raw-frame probe, read-only inventory and temporary monitor-mode runner
are diagnostic tools. Their success does **not** qualify OpenHD compatibility,
FEC performance, transmitted MCS/NOACK behaviour, video latency or stability.
`wireless_qualified` remains false until independently measured acceptance.
Do not add a generic AIC allowlist or enable a transmitter service to bypass
this gate. A single Q6A cannot establish over-the-air delivery.

The checked board has AIC8800D80 USB ID `a69c:8d81`, kernel
`7.0.11-6-qcom`, and a UVC camera. That camera offers 1080p30 in **MJPEG**;
its uncompressed 1080p formats only support 5 fps. Short local capture and H.264
hardware encoding trials succeeded; their timing findings are in `STATUS.md`.

## Build

Normal application selection:

```sh
scripts/qcom-app validate --app app013-openhd --machine radxa-dragon-q6a
```

The current checkout has missing eSDK buildtools. Use the independent,
containerized full-build path instead of restoring unrelated deletions:

```sh
apps/app013-openhd/scripts/build parse
apps/app013-openhd/scripts/build isolation
apps/app013-openhd/scripts/build tools
apps/app013-openhd/scripts/build driver
apps/app013-openhd/scripts/build upstream
```

Use kas-container 5.0 and the pinned image in `config/upstream-lock.json`.
Docker must pass `docker run --rm hello-world`. `KAS_CONTAINER` may point to
an installed wrapper. `KAS_WORK_DIR`, `DL_DIR`, and `SSTATE_DIR` are honoured;
defaults are outside the SDK under `/data/q6a-openhd/`. Existing downloads
and sstate are read-only mirrors. The build stores the complete local metadata
hash inventory in `baseline-lock.json`; no eSDK locked signatures are copied.
Selecting this app's private layer enables experimental monitor TX. Other apps
retain the ordinary driver configuration. This app's kas config explicitly
includes its private `meta-openhd` layer; the shared apps and board recipes
are unchanged. The private driver append adds patches/build flags only when
both `QCOM_APP=app013-openhd` and `MACHINE=radxa-dragon-q6a`. Upstream recipes
are unavailable to other app selections. The loaded driver exposes the
read-only `/sys/module/aic8800_fdrv/parameters/openhd_monitor` build marker;
it proves the compiled entry point only, not firmware capabilities.

The private selected-app append assigns this development baseline package
version `1:0.1.0-r0`; epoch 1 replaces the shared recipe's unordered hash
versions. Input digests still invalidate build tasks and are installed in
`/usr/share/qcom-selected-app/q6a-openhd/build.json`. Bump PV or PR before
releasing an updated package. Other app selections retain their original
version variables and task hashes; the isolation check covers both appends.

The upstream recipes pin Git commits for the core, sysutils and all four
submodules. They build development executables without startup
services. The Q6A/AIC capability profile remains gated by P1; this baseline
does not yet provide a qualified onboard transmitter. Sysutils uses a stable
source-derived version rather than incrementing it on each build.

Host verification:

```sh
cmake -S apps/app013-openhd -B /data/q6a-openhd/host
cmake --build /data/q6a-openhd/host
ctest --test-dir /data/q6a-openhd/host --output-on-failure
```

## Board inventory and raw-frame trials

Manage the Q6A over Ethernet or serial. Preserve its original modules and
firmware before installing a kernel-matched experimental driver. Keep the
known boot image and do not flash QSPI. Never force-load a module whose kernel
release or symbol versions do not match the board.

```sh
q6a-openhd-check --output /var/tmp/openhd-inventory.json
```

An explicit temporary driver trial can be run after checking its kernel
provenance and saving rollback artifacts:

```sh
q6a-openhd-driver-trial --module /var/tmp/aic8800_fdrv-monitor.ko \
    --expected-sha256 <verified-module-sha256> --output-dir /var/tmp/driver-trial-new
```

This requires Ethernet SSH, a verified onboard USB device and an active saved
Wi-Fi connection. It uses normal module loading, waits for udev/NetworkManager
discovery, attempts one second of diagnostic TX and restores the original disk
module, its parameters and the saved connection. Required helpers and probe
permissions are checked before module changes. It does not replace files in
`/lib/modules` or change firmware. A successful local send is not RF acceptance.

The independent receiver must already be in monitor mode on 5180 MHz/HT20,
with a supported driver and local regulatory permission. Generate a fresh
32-hex-digit run ID and use it on both ends. Start the receiver first:

```sh
# Independent ground station:
q6a-openhd-probe rx --interface mon0 --run-id 0123456789abcdef0123456789abcdef \
    --seconds 40 --output /var/tmp/raw-rx.json --capture /var/tmp/raw-rx.pcap
# Q6A, using Ethernet management and an experimental driver:
q6a-openhd-monitor -- q6a-openhd-probe tx --interface wlan0 \
    --run-id 0123456789abcdef0123456789abcdef --seconds 30 \
    --mbps 12 --payload 1200 --mcs 3 --output /var/tmp/raw-tx.json
```

Use fresh run IDs and output paths; reports cannot overwrite earlier trials.
The sender paces packets and does not release catch-up bursts. The receiver
rejects truncated or corrupt diagnostic payloads, filters local outgoing
packets, counts duplicate/reordered frames, and never compares monotonic
timestamps from different machines. TX uses OpenHD's qdisc-bypass socket option
and records errors by errno; any send error produces `TX_SOCKET_ERRORS` and a
nonzero exit status. Sender success is `TRANSMIT_ONLY`;
receiver success is `PEER_OBSERVATION`, not wireless qualification.

The optional RX capture preserves the original Radiotap and 802.11 bytes of
all valid frames matching the run ID, including duplicates, after filtering
local outgoing traffic. It uses PCAP v2.4 (link type 127) for examination with
tcpdump/Wireshark; it does not manufacture receiver metadata from requested TX
settings. A 256 MiB file cap or write/sync failure stops the run with a nonzero
exit status. Records larger than the receive buffer are rejected. The report
includes capture counters and Linux packet-socket drop statistics; unavailable
statistics must not be interpreted as zero drops. Socket statistics count
all traffic reaching that socket, including unrelated frames.

Preserve receiver identity, driver/firmware versions, `iw dev` channel/type,
regulatory readback, TX/RX reports and capture hashes for the same run ID.
Inspect received MCS/HT20 and bad-FCS/retry indications in an independent
decoder. If the receiver does not supply the necessary Radiotap metadata,
those settings remain unverified. RX Radiotap does not establish whether the
transmitter firmware honoured TX_NOACK; that gate requires separate on-air
ACK/retry behaviour evidence. This filtered capture excludes control/ACK
frames, so use a separate full monitor trace for that check.
PCAP timestamps use the receiver's realtime clock at userspace reception;
they are neither hardware timestamps nor synchronized cross-host latency.
`capture_complete` describes saving frames delivered to this socket, not
loss-free over-the-air delivery or completion of an interrupted trial.

`q6a-openhd-monitor` checks the board, USB ID, loaded feature marker,
regulatory state, and SSH route. It temporarily unmanages **only** the target
Wi-Fi device and its idle P2P companion after waiting for the target supplicant
interface to be removed. It reads back monitor mode and the requested HT20
channel before starting TX, restores both devices' management policy and the saved connection,
and terminates the child command before restoration on timeout/cancellation.
SIGKILL, power loss and hardware failure cannot execute restoration; recovery
uses Ethernet/serial and the saved connection UUID. It never stops
NetworkManager globally or changes TX power.

## Remaining acceptance stages

Local video preflight can run independently of the receiver:

```sh
q6a-openhd-video-preflight --source test --output-dir /var/tmp/openhd-test-new
q6a-openhd-video-preflight --source uvc --output-dir /var/tmp/openhd-uvc-new
```

Each trial requests 300 source frames and records capture, encoder-input and
encoder-output buffers separately, with PTS and local arrival times. It saves
the pipeline, log, bitstream hash and pending checks. Output directories must be new.
The encoder's `device` property is read-only on this board; the tool verifies
its default `/dev/video33`. Preliminary bitstream analysis showed that
`video_gop_size=15` produces a 16-frame keyframe interval, so the trial requests
14 and requires bitstream verification of the resulting 15-frame interval.
The trial disables `time_delta_based_rc`, which caused the preliminary UVC
stream to exceed the requested bitrate, and requests the low-latency mode.
Controls and both V4L2 queue frame rates are read back using the encoder's active
file descriptor. These local trials do not measure displayed fps, wireless
delivery or glass-to-glass latency.
Analyze a copied trial directory on the host, where FFmpeg/ffprobe are available:

```sh
apps/app013-openhd/scripts/analyze-video --trial-dir /data/q6a-openhd/reports/openhd-uvc-v5 \
    --output-dir /data/q6a-openhd/reports/openhd-uvc-analysis-new
```

The analyzer verifies the saved bitstream hash, complete decode/frame counts,
capture/input/output PTS correspondence, progressive VUI timing, no B frames,
GOP 15 and in-band SPS/PPS before each IDR. It preserves the raw FFmpeg reports.
The checked stream has a 60 Hz VUI tick rate and 30 progressive frames per
second; `ffprobe`'s `60 tbr` estimate is not the measured capture/display rate.
No bitstream timing fields are rewritten. This is host bitstream verification,
not ground-station or end-to-end acceptance.

1. P0: build baseline upstream OpenHD/sysutils, application and experimental
   driver; retain versions, hashes and rollback artifacts.
2. P1: verify independent unassociated raw-frame TX/RX, actual MCS/HT20/NOACK,
   payload integrity and driver/firmware stability. Firmware limitations stop
   advancement and require a minimal reproduction for the vendor.
3. P2: add a Q6A/firmware-specific capability profile in OpenHD/sysutils only
   after P1 passes; preserve the native encrypted/FEC wire protocol. Verify
   >=12 Mbps effective OpenHD/FEC payload for 30 minutes.
4. P3: integrate an OpenHD-managed Q6A hardware H.264 pipeline, first realtime
   test source, then UVC MJPEG decode to NV12. Validate codec controls and
   actual bitrate, disabled B frames, GOP 15 and periodic SPS/PPS. Never
   silently substitute software encoding or upscale 5 fps capture to 30 fps.
5. P4: test each source for 30 minutes at >=29.7 displayed fps without freezes
   exceeding one second, >=300 independent glass-to-glass latency samples,
   <=3-second link recovery, 2-hour soak, and ten cold boots. Package the
   image/service only after qualification, with rollback and bounded logs.

Local encoder or raw-socket observations must not be reported as completed
wireless delivery, end-to-end latency, long-term stability or flight testing.
