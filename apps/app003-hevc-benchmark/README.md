# HEVC hardware benchmark

Q6A application defaulting to 1920x1080, 30 fps, HEVC Main 8-bit, with GTK3 preview and
headless automation. Default bitrate is 8,000,000 bit/s; warmup is 5 seconds and
the measurement window is 60 seconds. Hardware results require the target board.

FPGA 设计、单路真实场景性能基线和 GOLDEN 测试资源说明见
[Q6A HEVC 编解码性能与 FPGA GOLDEN 参考](docs/fpga-golden-reference.md)。

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
* Without `--input-hevc`, decoder input is a hardware-generated 60-frame HEVC clip, parsed into access
  units, saved as `input.h265`, preloaded in RAM and repeatedly fed with continuous
  PTS. Corpus generation and disk writing precede the measured decoder session.
  The corpus starts at an intra frame and includes parameter sets. Memory is
  limited to 128 MiB. Decode mode requires a working hardware encoder to prepare
  this reproducible input. This short synthetic clip does not represent every
  content/bitrate combination.
* Y-PSNR uses `10*log10(255^2/MSE)` on the configured visible original and decoded Y code
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
and, for realtime tests, measured throughput >=99% of the configured frame rate.
Unpaced runs report measured capacity without an arbitrary FPS pass threshold.
Finite `--frame-limit` runs require exactly the requested frame count. Throughput
and bandwidth are valid only when the complete configured time window elapses;
short finite runs provide frame accounting and offline quality only. Loopback also requires matched Y-PSNR samples. The
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
The comprehensive suite uses schema version 2, retains existing metric names,
and adds configuration, finite-run validity and raw fixed-window byte counts.

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
（loopback 按配置帧率实时供帧，独立编解码不节流）。realtime 使用实时源或单调时钟
限速 appsrc，不能仅修改 PTS 来模拟 30 fps。`--input-nv12 FILE`（或 `--input-raw`）读取配置尺寸的紧密排列原始帧；
支持任意非零完整帧数，以最多 4 帧的队列预取并循环供帧。报告记录分块
SHA256、语料帧数、读取等待与队列最高水位。输入在测试期间必须保持不变。
独立解码默认先生成硬件 HEVC 语料；完整扫描使用 `--input-hevc` 外部语料。

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


## 完整性能扫描与 FPGA 私有码流对标（schema v2）

新增 CLI：`--width W --height H --fps NUM/DEN --pixel-format NV12|NV21`，
`--input-raw FILE`、`--input-hevc FILE`、`--frame-limit N`、
`--save-bitstream 0|1`、`--bitrate-mode default|vbr|cbr`、
`--pattern ball|snow|smpte`。NV12/NV21 均为 8-bit；Main10 必须通过真实
外部码流和设备能力确认，不能用 8-bit 输出或插件名称推断 10-bit 支持。
原有默认 GUI/CLI 仍是 1080p30。宽高必须为偶数；CLI 校验范围不代表设备支持范围。

外部 HEVC 文件要求相邻 `FILE.index`，每行为
`byte_offset byte_size presentation_index`，按解码顺序排列、完整覆盖文件，
显示序号必须为 0..N-1 的排列。语料应以随机访问帧开始、闭合 GOP，包含参数集。
工具按显示序号生成 PTS，支持 B 帧重排；压缩语料最多 128 MiB。
主机 `analyze-quality --index FILE` 使用 FFprobe 建立并验证这个索引。

`--save-bitstream 1` 在报告目录写入 `output.h265` 和对应索引，包括预热及排空。
保存磁盘 I/O 属于该次测量的开销；正式吞吐测试不保存码流。
`--frame-limit` 的用途是精确消费一段原始素材，质量评估不允许语料循环或截断。
该模式仍受 `--seconds + --warmup + 15` 秒的看门狗保护，秒数应足够完成全部帧。

板端入口（需要独占编解码设备，受控性能档需要 root）：

```sh
python3 run-suite --binary ./hevc-benchmark --output ./matrix \
  --phase full --decode-corpora ./decode-corpora --vpu-hz 460000048
```

频率必须来自当前板的 OPP 表；示例值不是所有平台通用的值。
运行前枚举能力和温度，默认/性能策略交替，原有热保护保留；视频温度达到
75°C 即停止当前试验。退出、取消及异常会终止子进程并恢复 governor/VPU 投票。
SIGKILL/断电不能执行恢复。程序不关闭其他编解码用户，不修改启动配置。

`--phase probe|smoke|scan|full|custom` 可单独运行；full 包括 480p 到驱动
4096×4096 边界的 24/30/60/120 fps 扫描、各分辨率最高达标与首个失败点、
默认/性能档各三轮 60 秒正式测试、1/2/4/8 会话并发，以及代表点 600 秒稳定性。
解码使用外部语料，编码和回环容量扫描使用小球；必须结合内容测试解释上限。
`--phase custom --cases cases.json` 接受 trial 参数列表，例如：

```json
[{"config":{"width":1920,"height":1080,"fps":60,"mode":"encode","pacing":"realtime","input_raw":"/path/natural.nv12","bitrate":8000000,"bitrate_mode":"vbr"},"setting":"performance","repeat":0,"seconds":60,"warmup":5,"count":1}]
```

同一输出目录可续跑，按参数、输入/二进制/运行器 SHA256 识别已完成试验。
完整失败结果保留；取消或未完成试验重新运行。检查 `inventory.json`、
`selected.json`、`index.json` 和每次 `trial.json`；每个进程还有完整 `report.json`。
并发各路有各自的固定测量窗口，启动时间略有差异；汇总须注明相加 FPS 的口径。

主机语料和画质命令（需要 FFmpeg/FFprobe，使用独立软件解码）：

```sh
python3 scripts/prepare-corpus --decoder-matrix --output decode-corpora
python3 scripts/prepare-corpus --output raw-corpora --width 1920 --height 1080 \
  --fps 60 --frames 120 --natural-source natural.mov --provenance source-manifest.json \
  --decode-fixtures
python3 scripts/analyze-quality --report trial/encode-ID/report.json \
  --raw raw-corpora/natural.nv12 --output quality
```

素材包含运动、固定随机种子的复杂纹理、切换场景和自然画面。自然素材保留
连续源帧，按指定帧率重放；这不证明原始拍摄帧率。manifest 保存转换命令、
素材出处和哈希。短片循环的持续性能与长段非重复真实视频性能必须分开表述。
画质校验要求源文件哈希、编码/解码帧数及保存 AU 的显示次序完全对应；允许
解析器把四字节起始码首个零字节划到上一 AU，不允许跳过其他字节或错配帧。
输出 Y/U/V pooled PSNR、逐帧 PSNR、Y-SSIM、实际平均码率、I/P/B 帧和关键帧位置。
数值来自 FFmpeg 日志精度；不转换像素值范围，不设主观质量合格阈值。

带宽记录来自所有编码器输出缓冲，包括无关联 PTS 的头部；保存 100 ms 原始
字节桶，派生不重叠 100 ms/1 s 的最小、均值、峰值，空完整窗口计零。
无完整时间窗口时 `bandwidth_valid=false`。它不是网络或 DDR 带宽。
CPU/RSS 是应用开销；延迟包含驱动与队列；频率不等于利用率或电功率。
FPGA 对标使用实际码率、画质、像素吞吐、延迟与突发码流，不据此推算 LUT/DSP/BRAM。


### 原始帧色度布局验证

原始文件是紧密排列的 NV12/NV21。板端供帧将它逐平面复制到 system DMA heap，
使用线性 stride 128 字节、Y 32 行、UV 16 行对齐，经 GstVideoMeta 描述并直接
DMABUF-import。布局和输入等待写入报告；分配/复制的应用开销仍包含在整条管线中。
没有 DMA heap 或导入协商失败时显式失败，不回退到未经验证的 MMAP 原始输入。

本次 1080p 的 MMAP 预检发现 UV 偏移 8 行；仅添加紧密 VideoMeta 不足以修复。
对齐 DMA-BUF 的相同素材验证保持编码可见尺寸 1920×1080，并消除了色度偏移。
因此旧路径的 Y-PSNR 不能作为完整彩色质量验收。吞吐扫描的小球源和原始输入
属于不同的供帧路径，报告需明确区分。完整质量评估始终校验 Y/U/V 三个分量。

### 连续真实场景测试

小球、噪声和短片循环仅用于诊断或稳定性施压，不能代表连续真实场景性能。
正式基线使用完整连续拍摄素材，保存来源、许可、转换命令、真实拍摄帧率和原始帧
SHA256。例如 3600 帧 1080p60 航拍，预热 5 秒、统计 55 秒，并设置
`--frame-limit 3600`，不循环、补帧或插帧；原始 60000/1001 fps 素材按 60 fps
重放时必须披露 1.001 倍速。65 秒山景可使用 5 秒预热、60 秒统计。

`--input-raw` 也接受以 `.zst` 结尾的单帧 Zstandard 无损压缩原始文件
（例如 `zstd -1 input.nv12 -o input.nv12.zst`，必须包含已知内容大小）。
它以最多 4 帧预取、流式解压供帧，不把整段原始视频放入内存。启动前完整校验
解压后的原始字节 SHA256；正式测量包含解压、读取、DMA 分配与复制开销，
`raw_read_wait_ms` 和进程 CPU 用于识别供帧瓶颈。不要将结果称为纯 VPU 内核速度。
截断、损坏或拼接压缩帧明确失败。主机画质分析仍使用未压缩的同源原始文件。

```sh
./hevc-benchmark --mode loopback --width 1920 --height 1080 --fps 60 \
  --input-raw drone-1080p60.nv12.zst --frame-limit 3600 \
  --warmup 5 --seconds 55 --pacing realtime --quality off \
  --bitrate 8000000 --bitrate-mode vbr --time-delta-rc off \
  --save-bitstream 1 --output real60
```

通过 `run-suite --phase custom --cases cases.json` 对相同连续素材执行默认与受控
性能档、独立编码/解码和回环、多码率与重复试验。先保存一次完整码流用于逐帧
Y/U/V PSNR、Y-SSIM 验证，再关闭保存进行吞吐测量，区分两者的 I/O 开销。
未达到实时门限但完整 EOS 的码流可以分析画质，分析文件保留原始失败状态；
画质结果不会把吞吐 FAIL 改为 PASS。长稳测试重复真实素材时必须显式标注循环。

### GStreamer 时间戳与码率控制

新增 `--time-delta-rc default|off|on`，默认保留原驱动配置，显式设置时校验当前
编码器 FD 的 `time_delta_based_rc`（0x0099200d）读回值。GStreamer 1.28.2 的
`gstv4l2videoenc.c` 将 system_frame_number 传入 buffer pool；后者将这个帧序号
写入 V4L2 timestamp.tv_sec，它并非素材的逐帧时间。Qualcomm 驱动按时间差控制
码率时，这个约定可能导致目标码率与输出严重偏离。对固定帧率回放用
`--time-delta-rc off` 做实测对照，保存该设置、实际码率和逐帧画质后再作判断。
不得根据控制项读回成功就声称码率达标，历史默认路径的测量也必须保留区别。

所有延迟以 `reference` 输入探针及编解码器 sink/src 探针的单调时钟计算；
包含驱动排队，原始文件读取、解压和 DMA 分配/复制发生在输入探针之前，
它们会影响供帧和吞吐，但不计入该探针到解码输出的延迟。它不是摄像头到屏幕
延迟，也不是硬件内部逐周期延迟。进程 CPU 含预热/运行/排空，启动前 SHA256
计算不计入；RSS 是进程生命周期峰值。1 秒系统采样同时保存温度、CPU 和 VPU 时钟。

内置 `run-suite` 的 smoke/scan/full 编码用例显式设置时间差 RC 为 off；
custom 用例尊重 JSON 的选择，CLI/GUI 的 default 仍保留驱动默认值以便历史对照。

### Shared video library

Host video assets are now stored under `/video`, independently of the SDK at
`/data/qcom_sdk`. Use `/video/metadata/baseline.json` for the exact frozen input
IDs, content hashes, relative paths and recovery status. Join each relative path
with `/video`; an unavailable baseline must not be replaced with a similar clip.

```sh
python /video/tools/library.py list
python /video/tools/library.py lookup /home/dsh/Desktop/qcom_sdk/artifacts/hevc-full/real-decode/drone-1080p60-b.h265
```

For host corpus generation, pass the resolved library source to
`prepare-corpus --natural-source` and its provenance record to `--provenance`.
For board runs, copy the selected asset to the explicitly configured board input
path; `/var/lib/hevc-benchmark/...` names still denote board storage.
Historical board reports stay under `/data/qcom_sdk/artifacts`; relocated saved
video outputs and their AU indexes are mapped by `/video/metadata/migration.json`.
No compatibility symlinks are created for old host media paths.
