# HEVC hardware benchmark

Q6A application for 1920x1080, 30 fps, HEVC Main 8-bit, with GTK3 preview and
headless automation. Default bitrate is 8,000,000 bit/s; warmup is 5 seconds and
the measurement window is 60 seconds. Hardware results require the target board.

## Build and SD image

From the SDK root in a fresh shell:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app app003-hevc-benchmark --machine radxa-dragon-q6a
scripts/qcom-app build --app app003-hevc-benchmark --machine radxa-dragon-q6a
scripts/qcom-app image --app app003-hevc-benchmark --machine radxa-dragon-q6a
```

The selected image is `qcom-multimedia-proprietary-efi-sd-image`. Its `.wic` and
`.wic.bmap` are under `tmp/deploy/images/radxa-dragon-q6a/`. Select the intended
removable SD card explicitly when using a WIC-compatible image writer. Do not
write to the workstation system disk. The app does not flash media or do OTA.
The normal Q6A boot-firmware prerequisite remains unchanged.

## Run

Open **HEVC 1080p30 Benchmark** from Weston, or run:

```sh
hevc-benchmark --gui
hevc-benchmark --mode loopback --seconds 60 --output /var/tmp/hevc
hevc-benchmark --mode encode --seconds 60 --output /var/tmp/hevc
hevc-benchmark --mode decode --seconds 60 --output /var/tmp/hevc
hevc-benchmark-acceptance /var/tmp/hevc-acceptance
```

No arguments opens the GUI; CLI runs without a display. Stop/Ctrl+C cancels the
run and saves a CANCELLED report. No stress test starts automatically at boot.
Options: `--warmup 0..60`, `--seconds 1..600`, `--bitrate 100000..80000000`,
`--encoder v4l2h265enc`, `--decoder v4l2h265dec`, `--output DIRECTORY`.
Device-specific V4L2 HEVC factory names may be supplied when multiple devices are
registered. Software factories are rejected. Missing devices/plugins, denied
permissions, negotiation errors and missing timestamps fail explicitly.

## Measurements

* Loopback uses GStreamer's deterministic moving-ball source with NV12/BT.709.
  Actual input luma is retained, matched by PTS, and compared to decoded luma.
  The reference queue is bounded at 120 frames. Linear mappable NV12 is required;
  UBWC buffers must never be interpreted as linear pixels. Video frame stride
  and crop metadata determine the visible Y region.
* Encode/decode latency is the monotonic time between a codec's sink and source
  pad for the same PTS. It includes driver/hardware queueing, excludes source
  reference-copy time, and is not pure hardware-core execution time. Reported
  mean/P50/P95/max use frames whose source timestamp lies in the measurement
  window. The end-to-end metric starts after the reference copy.
* Throughput is the number of codec outputs in a fixed wall-clock window after
  warmup, divided by the full requested duration. Input media caps of 30/1 do
  not pace the independent tests. Startup and drain are outside that window.
* Decoder input is a hardware-generated 60-frame HEVC clip, parsed into access
  units, saved as `input.h265`, preloaded in RAM and repeatedly fed with continuous
  PTS. Corpus generation and disk writing precede the measured decoder session.
  The corpus starts at an intra frame and includes parameter sets. Memory is
  limited to 128 MiB. Decode mode requires a working hardware encoder to prepare
  this reproducible input. This short synthetic clip does not represent every
  content/bitrate combination.
* Y-PSNR uses `10*log10(255^2/MSE)` on 1920x1080 original and decoded Y code
  values, without range conversion. Reports contain each frame, the distribution
  of frame PSNR and PSNR calculated from pooled squared error. Exact matches are
  the JSON string `"Infinity"`; absent measurements are `null`. Chroma is not
  scored. No quality threshold is asserted.
* Quality calculations run only in loopback. GUI preview has an independent
  bounded leaky queue; preview skips are separate from codec missing frames.
  The GTK update timer runs every 16 ms. Reports distinguish preview branch
  frames, GTK image submissions, pending frames and frames coalesced before a
  UI update. These are application counters, not physical HDMI scanout proof.
  GUI conversion/display uses CPU and can contend with codec feeding. Use the
  headless acceptance command for the independent throughput baseline.
* CPU percent covers the measured session including startup/drain and uses 100%
  per CPU core. Peak RSS is the process lifetime high-water mark (and can include
  an earlier GUI run or decoder corpus preparation). Neither is isolated codec
  hardware utilization.

Each invocation writes a uniquely named directory containing `report.json` and
`run.log`. Reports identify platform, GStreamer, hardware factory/device, actual
caps, V4L2 driver identity, requested bitrate, frame accounting, preview skips, timing, quality and
pipeline errors. Bitrate is requested through `video_bitrate`; actual encoded
quality/caps must be checked on the board. An encoder rejecting this control is
a failure, not an ignored software fallback.

PASS requires a completed measurement window, valid timing order, error-free EOS drain, no missing/duplicate/unmatched codec frames,
and measured throughput >=30 fps for an independent codec, or >=29.7 fps for
both codecs in loopback. Loopback also requires matched Y-PSNR samples. The
command exits 0 on PASS, 1 on failure and 2 on cancellation/invalid arguments.
PSNR and latency have no additional pass thresholds. Offline self-tests do not
establish hardware speed, HDMI behavior or boot acceptance.

## Offline tests

```sh
cmake -S apps/app003-hevc-benchmark -B artifacts/hevc-benchmark/core -DHEVC_CORE_ONLY=ON
cmake --build artifacts/hevc-benchmark/core
ctest --test-dir artifacts/hevc-benchmark/core --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests
```

The installed `hevc-benchmark --self-test` does not initialize GTK/GStreamer or
access hardware and is suitable for the image's QEMU rootfs check.

## Low-latency comparison

The default `--latency-profile baseline` preserves the original codec settings.
`--latency-profile low` requests low-latency mode on both hardware codecs and
explicitly disables encoder B frames and frame skipping. `encode-low` and
`decode-low` isolate each side for controlled experiments. All profiles retain
1080p30, the requested bitrate, GOP defaults, and full Y-PSNR checking.
`--operating-rate 30|60|120` requests decoder operating rate (Q16 driver control);
0, the default, leaves it unchanged. This does not change input frame pacing.
Unsupported controls, readback failures, and effective-value mismatches fail the
run. Readback uses the GStreamer session's device descriptor on first output.

```sh
hevc-benchmark-latency --binary /path/to/test/hevc-benchmark \
    --profile low --performance --output /var/tmp/hevc-low-trial
```

The Python runner creates a new output directory, samples CPU governors,
available frequencies and thermal sensors once per second, and stores
`trial.json` alongside the original application reports and console log.
`--performance` requires permission to write CPU cpufreq policies. It restores
all changed governors on normal exit, setup failure, SIGINT, SIGTERM and SIGHUP;
SIGKILL or power loss cannot execute cleanup. It never disables thermal limits,
changes GPU settings, or installs a persistent boot policy. A trial without
`--performance` preserves existing governors. No electrical power is measured.

Reports add `latency_profile`, `requested_operating_rate`, verified
`encoder_controls`/`decoder_controls`, and `actual_encoded_bitrate` (encoder
output bytes in the measurement window, including in-band headers). Individual
frames now contain `source_to_decode_ms` and `before_copy_to_decode_ms`;
the latter also has aggregate statistics and includes the reference luma copy.
Existing `source_to_decode_ms` retains its original post-copy starting point.
These additions preserve schema version 1 and all existing fields.

Choose configurations only after alternating repeated trials: normal EOS,
>=29.7 fps on both sides, no missing/duplicate/unmatched frames, and pooled
Y-PSNR degradation <=0.5 dB against the same-duration baseline. Compare actual
bitrate as well as its requested control; the moving-ball VBR stream can be far
below 8 Mbps. Rank the median P95 across three runs, using average latency then
the lower performance setting for differences below 1 ms. Confirm the winner
with a 600-second run. A low-latency control name alone is not evidence of an
improvement, and short synthetic clips do not represent arbitrary real video.

For the full screening, three-round comparison and ten-minute stability run:

```sh
hevc-benchmark-latency-matrix --binary /path/to/test/hevc-benchmark \
    --output /var/tmp/hevc-latency-matrix
```

This matrix includes performance-governor trials and requires root. It writes
`matrix.json`, `selection.json`, and `result.json`; each trial has its own raw
reports. The final stability quality check uses the first 1,800 measured frames
against the 60-second baseline, because the moving-ball content varies over a
longer sequence. Stability FPS, frame accounting and EOS use the entire run.
Available VPU clock rates are sampled read-only from debugfs; unavailable clock
information is omitted rather than inferred from the requested operating rate.

Timed raw-source runs stop at the source streaming boundary before counting the
next buffer, then send EOS behind all forwarded frames. Decoder appsrc runs use
its ordered end-of-stream API. The watchdog detects stalled drains; it does not
inject an asynchronous EOS that could overtake a counted input frame.

### 第二阶段：供帧、固定输入与驱动追踪

`--pacing auto|realtime|unpaced` 控制实际供帧节奏：auto 保持原有默认
（loopback 实时 30 Hz，独立编解码不节流）。realtime 使用实时源或单调时钟
限速 appsrc，不能仅修改 PTS 来模拟 30 fps。`--input-nv12 FILE` 预加载恰好
60 帧 1920×1080 紧密排列 NV12 BT709 数据（186624000 字节），循环供帧，
报告记录 SHA256；独立解码先用同一原始输入生成硬件 HEVC 语料。

在主机运行 `scripts/generate-latency-inputs --output DIR` 生成平移棋盘和固定
种子的运动纹理。保留 manifest.json 与源文件哈希；每次进程只预加载一个语料。
逐帧报告新增 source_index、Y 平方误差/像素数、应用各阶段的单调时钟时间。
`--quality off` 仅用于测量 PSNR 处理干扰，报告 quality_validated=false，不能
作为完整验收结果；默认仍为 full，所有质量样本必须完成后才能通过。

板端受控运行示例（输出目录必须不存在）：

```sh
python3 run-latency --binary ./hevc-benchmark --output trial \
  --profile decode-low --operating-rate 30 --performance \
  --pacing realtime --vpu-hz 335000000 --seconds 60
```

`--vpu-hz 0` 保留驱动自动投票。非零值必须来自运行设备树 OPP 表；工具拒绝
已有 VPU 会话，在启动前及运行中检查 video-thermal 低于 75°C，退出时恢复
原始全局投票和 CPU governor。平台原有热保护保留。该控制仅是显式基准测试
选项，并未成为驱动或产品默认配置；请独占编解码设备进行测试。频率和温度
采样写入 trial.json，不代表整机功率测量。

`--trace` 在独立 ftrace 实例中记录 VIDC/V4L2 事件，使用 mono 时钟；结束后
保存 trace.txt 和每 CPU 丢失统计，任何 overrun/dropped 均拒绝该追踪结果。
使用 `scripts/analyze-latency-trace TRIAL_DIR` 关联帧：应用提交/输出时间窗口内
唯一的驱动 INPUT qbuf，并以该编解码器下一次应用提交限制窗口，支持多帧重叠；
再匹配相同会话及驱动时间戳的固件 OUTPUT 响应。
驱动时间戳经过重映射，不能直接当成应用 PTS。qbuf 到固件响应包含驱动缓存
处理、固件执行及中断响应，不能称为纯硬件核心时间。追踪使用短测试，正式
性能结论使用关闭追踪的 60 秒重复与 600 秒稳定性测试。

主机脚本 `run-latency-stage2` 可复制到板端，与 run-latency 同目录运行：

```sh
python3 run-latency-stage2 --binary ./hevc-benchmark --inputs ./inputs \
  --candidate-hz 335000000 --output matrix
```

脚本在 ball、checkers、texture 三种输入上交替运行自动投票基线与候选各三轮，
比较 P95 中位数和同 source_index 的 pooled Y-PSNR（允许降幅 ≤0.5 dB）；
任何输入 P95 回退超过 1 ms 则拒绝通用推荐。通过后执行 600 秒 ball 稳定性
及实时独立编解码。候选频率应先经档位筛选；不足 1 ms 的差异优先选低档位。
