# Q6A WCD9385 headphone audio

The isolated PAL headphone test was audibly accepted by the board operator at
20% volume on 2026-09-19. The integrated desktop now exposes one
`Q6A WCD9385 Headphones` output. The operator also confirmed normal desktop playback at 20%. Repeated physical
hotplug and the 30-minute soak are separate acceptance gates below.

## Fix and scope

The Q6A image had the X1E80100 `card-defs.xml`: the upstream configuration
Makefile installs several platform definitions to the same destination. The
machine append explicitly selects QCM6490 virtual PCM definitions. AGM now uses
the selected sound card's short name, matching PAL's configuration lookup.
This avoids the board-vendor/version-dependent long-name ACDB directory.

The board has no supplied vendor calibration package. The Q6A configuration
is derived from upstream QCS6490 RB3 Gen2 data, with explicit Q6A filenames.
The verified scope is stereo playback to the WCD9385 3.5mm jack. This does not
certify speaker, microphone, headset buttons, HDMI, or all calibration keys.
Reference provenance:

| Component | Upstream revision / input |
| --- | --- |
| AudioReach conf | `7143e0fe9278b81de7276a372d4595fff36167e8`, `qcom/qli/qcm6490/card-defs.xml` and `acdbdata/` |
| PAL | `2fa9b824b68f8ab4f573301820aa3c06ce1e2c83`, `configs/qcom/IoT/qcs6490/*QCS6490_RB3Gen2.xml` |
| AGM | `b5587a73c25085268aad89839fea04906063385d`, plus the two layer patches |
| PipeWire PAL plugin | `f7eec1c5e5aae9be82afab2aac2f4670d674a93b`, plus headphone policy patch |

AGM retries the ATS control thread with ordinary scheduling if the desktop
lacks permission to create the existing FIFO-scheduled thread. Other
failures remain errors. This does not claim working DIAG: optional ATS transport
initialization still retries and logs failures on the tested image.

Only the desktop user's PipeWire/PAL instance owns the audio hardware. The
Q6A PipeWire and WirePlumber package configuration omits system services.
`q6a-audio-access`, pulled in by qcom-xfce, grants the active seat access to
`aud_pasthru_adsp` and `msm_audio_mem` using udev `uaccess`; it does not make
these devices world writable or add users to groups.

The plugin exposes one low-latency stereo headphone sink, reports
`q6a.jack.connected`, suspends on removal, and reports PAL startup failures.
It accepts both headphone and line-out insertion events. The tested plug is
classified as Line Out: `Headphone Jack = off` alone is not proof of a fault.
The generic policy that redirects unplugged headphones to a speaker is disabled.

The music player checks PulseAudio's selected sink and the sink actually used
by its stream. Null outputs, unplugged outputs, service loss, removed sinks,
and stream movement stop playback with a recovery message. A recovered output
requires the user to play again. The device list refreshes automatically and
the stale output error clears when the selected device becomes available;
other decoding errors remain errors. PAL's legitimate virtual sink remains usable.

## Running-board deployment and rollback

The tested board runs kernel `7.0.11-6-qcom` and has an immutable `/usr`.
Component packages were built and checked, but the running OSTree deployment
was not replaced. User-space changes are installed as follows:

- `/var/lib/q6a-audio/runtime-v1/usr/lib/` contains the new AGM library and PAL
  PipeWire module. A user-service drop-in sets their library/module search paths.
- `/etc/card-defs.xml`, Q6A-named ACDB and PAL XML files provide configuration.
- `/etc/udev/rules.d/70-q6a-audioreach.rules` grants active-seat access.
- `/etc/pipewire/q6a-pipewire.conf` and its `.d/` directory provide an independent
  configuration; `/etc/wireplumber/q6a-wireplumber.conf` and its `.d/` directory
  provide the corresponding policy. User-service drop-ins select these names.
  Independent names are needed on this image because ordinary drop-ins append
  module arrays and retain the original generic PAL modules.
- System `pipewire`, `pipewire-pulse`, `wireplumber` services and the three
  PipeWire/Pulse sockets are disabled and masked. Their prior state is backed up.
- The player is `/var/lib/q6a-music-player/releases/0.1.0-8abc0890`, selected by
  `/var/lib/q6a-music-player/current`. Previous releases remain available.

Backups are in `/var/lib/q6a-audio/backup-20260919-024009`. The saved unit states,
original files, and newly introduced paths are recorded separately. To restore
this board's original audio setup:

```sh
python3 /var/lib/q6a-audio/rollback.py /var/lib/q6a-audio/backup-20260919-024009
```

This stops the player and user audio services, removes introduced configuration,
restores original `/etc` files and system unit state, reloads user services, and
starts the former setup. The original `/usr` was never changed. Restore the
player's previous release symlink separately if desired. Reboot persistence has
not been tested; no kernel, DTB, firmware, flash, image update or reboot occurred.

## Verification

Component packages and QA passed for conf, AGM, PAL, the PAL plugin, access
rules, PipeWire, WirePlumber, and the selected player. Final build: 3,256 tasks,
all succeeded. Host CTest passed 5/5, including null-output detection and an
isolated Pulse disconnect/reconnect. The audio audit unit tests passed 6/6.
The integrated board prerequisite audit passed; it does not measure sound.

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app app012-music-player --machine radxa-dragon-q6a
python3 -m unittest discover -s layers/meta-radxa-dragon/scripts/tests -p test_q6a_audio.py -v
python3 layers/oe-core/scripts/contrib/patchreview.py -v layers/meta-radxa-dragon
```

Select `MACHINE = "radxa-dragon-q6a"`, `QCOM_APP = "app012-music-player"` and
`QCOM_APP_IMAGE = "qcom-multimedia-proprietary-efi-sd-image"` in a BitBake
postread file and build the affected component `do_package_write_rpm` and
`do_package_qa` tasks. No image build is needed for these checks.

The host CMake build needs GTK3, GStreamer and libpulse development packages.
CTest also uses installed PipeWire/Pulse binaries to run isolated servers:

```sh
cmake -S apps/app012-music-player -B /tmp/music-player-build -DBUILD_TESTING=ON
cmake --build /tmp/music-player-build
ctest --test-dir /tmp/music-player-build --output-on-failure
```

Run the read-only collector on the board and check its JSON on the host:

```sh
python3 /tmp/audit-q6a-audio.py --pulse-user weston > /tmp/q6a-audio.json
python3 layers/meta-radxa-dragon/scripts/audit-q6a-audio.py --check q6a-audio.json
```

Docker smoke test passed. Podman and `kas-container` were unavailable, so the
required CI helper could not run oe-selftest. Local checks do not replace it.
The patch metadata scan found no missing/malformed Signed-off-by or
Upstream-Status fields.

## Physical acceptance record

- Isolated PAL: 48 kHz stereo S16, 30 seconds, volume 0.2; operator confirmed
  audible and normal sound. Completed writes alone were not used as acceptance.
- Jack input: two removal/insertion cycles captured on the board Headset Jack
  input, with `SW_LINEOUT_INSERT` and `SW_JACK_PHYSICAL_INSERT` changing 0/1.
- Desktop: one real PAL sink, jack connected, player stream explicitly targeting
  it, 20% application volume, and running sink verified. Pulse's cubic volume
  display is approximately 58% for linear gain 0.2; this is expected.
- Desktop audible confirmation: operator reported normal sound.
- 20 application play/stop cycles passed: each stop removed the application
  stream, and each start targeted the same running PAL sink. The one-second
  stop intervals do not certify full DSP graph teardown on every cycle.
- Integrated removal while playing: jack metadata changed to false and the
  player stream disappeared about 1 second later. Reinsertion left playback
  stopped, as intended. One such active-playback cycle is captured so far.
- Recovery UI regression fixed in player `8abc0890`: the output list updates
  automatically and the stale unavailable error clears after recovery. A board
  audio-service stop/start test showed the recovery prompt without automatic
  playback; an explicit Play then created a stream on the running PAL sink.
- At least 5 integrated physical hotplug cycles, each followed by manual
  playback: operator confirmed all normal on 2026-09-19. The event log separately
  captures removal, stream shutdown, reinsertion and no automatic playback.
- Continuous loop test stopped at the operator's request after 1,250.423
  observed seconds (20 minutes 50 seconds), with 247 samples. One sample
  during the first loop transition saw a temporary inactive/new stream and
  volume metadata; the next sample about 5 seconds later was normal. There
  were no sustained failures or process restarts. Eight automatic restarts
  of the 2:23 track were observed. PipeWire RSS ranged from 22408 to 23220 KiB.
  Audio-node error counters were zero at the start and approximately 5, 10,
  15 and 20 minutes; the 10-minute kernel log check had no new entries.
  This is an interrupted test, not a completed 30-minute soak.
- The operator ended further testing and requested a local commit. Playback
  and the monitoring processes were stopped. Boot persistence remains untested.
- The deployed player remains `8abc0890`; the subsequent README update only
  documents automatic output recovery and does not change executable code.
- Stereo channel isolation, alternate plugs and boot persistence: not tested.

Detailed logs, payload checksums, snapshots and screenshots are under the
ignored `artifacts/music-player/2026-09-19-wcd9385/` directory. Initial diagnostic
logs retain their historical state and are superseded by this acceptance record.
