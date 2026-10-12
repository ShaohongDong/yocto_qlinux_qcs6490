# Q6A HEVC 编解码性能与 FPGA GOLDEN 参考

本文记录 Radxa Dragon Q6A 当前单路 HEVC 硬件编解码表现、测试资源目录和
FPGA GOLDEN 建立方法。数据快照来自 2026-09-20 完成的真实场景板测。这里的
Q6A 数据用于确定吞吐、延迟、码率和画质量级，并不要求 FPGA 编码器生成与
Q6A 逐字节相同的码流。

完整板测报告位于 `/data/qcom_sdk/artifacts/hevc-full/REPORT.md`。
`/data/qcom_sdk/artifacts/` 被仓库根目录的
`.gitignore` 排除，因此克隆源码后不会自动获得这些大文件；使用本文前应取得
同一测试资源包并通过 manifest 中的 SHA256 校验身份。

## 1. 平台和测量口径

板端环境如下：

| 项目 | 值 |
|---|---|
| 板卡 | Radxa Dragon Q6A |
| SoC/设备树 | Qualcomm QCM6490，`radxa,dragon-q6a` / `qcom,qcm6490` |
| 内核 | Linux 7.0.11-6-qcom |
| GStreamer | 1.28.2 |
| 硬件编码设备 | `/dev/video33` |
| 硬件解码设备 | `/dev/video32` |
| 正式原始格式 | NV12，8-bit 4:2:0，BT.709 limited |
| 正式编码控制 | `time_delta_rc=off` |
| 性能档 VPU 投票 | 460000048 Hz |
| 实时通过门限 | 配置帧率的 99%，且帧计数、时间戳和 EOS 完整 |

实时测试按素材帧率供帧，验证一遍完整连续素材；`unpaced` 测试取消节流，在
固定统计窗口内循环同一真实素材，用于测容量上限。两者用途不同，不能把
`unpaced` PASS 表述为长时间稳定性或新增场景覆盖。

延迟起点为应用 `reference` 输入探针，终点为编码或解码输出探针，包含驱动
排队，但不包含输入探针之前的文件读取、Zstandard 解压、DMA 分配和复制。
因此本文延迟不是摄像头到显示器延迟，也不是 VPU 内部周期数。CPU 使用率按
单核 100% 计，RSS 不包含全部内核和 DMA 缓冲。

正式原始输入使用 system DMA heap，按驱动要求设置 128 字节 stride、Y 平面
32 行和 UV 平面 16 行对齐，并通过 DMABUF-import 送入编码器。旧 MMAP 路径在
1080 行输入上存在色度偏移，其质量数据不得作为 GOLDEN。

## 2. 当前单路性能基线

下表只包含单路结果。吞吐数值来自真实拍摄素材，性能档测试使用受控 CPU/VPU
设置；默认档实时解码保留系统自动调频。

| 规格 | 模式 | 节奏/档位 | 统计窗口 | 结果 | 判定 |
|---|---|---|---:|---:|---|
| 1920x1080@60 | 编码 | unpaced/performance | 55 s | 96.22–96.24 fps | PASS |
| 1920x1080@60 | 编码 | unpaced/default | 55 s | 95.98–96.18 fps | PASS |
| 1920x1080@60 | 独立解码 | unpaced/performance | 15 s | 286.80 fps | PASS |
| 1920x1080@60 | 独立解码 | realtime/default | 完整 3600 帧 | 60.02 fps | PASS |
| 3840x2160@30 | 编码 | unpaced/performance | 15 s | 45.93 fps | PASS |
| 3840x2160@60 | 独立解码 | unpaced/performance | 15 s | 75.33 fps | PASS |
| 3840x2160@60 | 独立解码 | realtime/default | 完整 3600 帧 | 60.00 fps | PASS |
| 3840x2160@30 | 编码后解码回环 | realtime/performance | 55 s | 编码 28.20、解码 28.22 fps | **FAIL** |

4K30 回环完整消费并输出 1800 帧，EOS 正常，无缺失、重复或无效时间戳；但
速度低于 29.7 fps 门限，所以必须保留为 FAIL，不能因帧完整而改写为实时通过。
其输入探针到解码输出 P99 为 152.56 ms。

独立解码码流包含 B 帧，输入探针到输出延迟包含重排序等待。1080p60 和
4K60 实时解码 P99 分别为 119.78 ms 和 126.42 ms；这些数值适合做系统排队
参考，不适合作为 FPGA 核内部流水线延迟目标。

### FPGA 吞吐目标解释

建议同时记录“功能实时线”和“Q6A 对标线”：

| 功能 | FPGA 功能实时线 | Q6A 单路对标线 |
|---|---:|---:|
| 1080p60 编码 | >=60 fps | 约 96 fps |
| 1080p60 解码 | >=60 fps | 约 287 fps |
| 4K30 编码 | >=30 fps | 约 46 fps |
| 4K60 解码 | >=60 fps | 约 75 fps |

Q6A 的 4K30 编码和独立解码分别超过实时线，但同一会话内的 4K30 回环没有
达到实时线。FPGA 若将编解码核同时集成，应单独验证共享 DDR、参考帧缓存和
仲裁后的回环吞吐，不能把两个独立容量数字直接相加。

## 3. 码率与画质锚点

以下结果均使用完整同源素材逐帧离线计算 Y/U/V PSNR 和 Y-SSIM，并显式设置
`time_delta_rc=off`。实际码率按完整保存码流字节数除以名义媒体时长计算。

| 输入 | RC | 请求码率 | 实际码率 | Y/U/V PSNR dB | Y-SSIM |
|---|---|---:|---:|---|---:|
| 1080p60 航拍 | VBR | 4 Mbps | 5.030 Mbps | 39.60 / 51.11 / 51.80 | 0.94960 |
| 1080p60 航拍 | VBR | 8 Mbps | 10.018 Mbps | 41.64 / 52.04 / 52.71 | 0.96793 |
| 1080p60 航拍 | VBR | 16 Mbps | 19.878 Mbps | 44.06 / 53.28 / 53.96 | 0.98048 |
| 1080p60 航拍 | CBR | 8 Mbps | 7.967 Mbps | 40.83 / 51.48 / 52.20 | 0.96204 |
| 1080p30 山景 | VBR | 8 Mbps | 4.766 Mbps | 49.66 / 52.88 / 53.67 | 0.99192 |
| 4K30 航拍 | VBR | 16 Mbps | 19.962 Mbps | 42.48 / 52.73 / 53.48 | 0.97238 |

Q6A VBR 默认存在 25% bitrate boost，所以请求 8 Mbps 时实测约 10 Mbps 属于
当前驱动配置的正常表现。比较 FPGA 码控时应使用实际完整码流码率，而不是只看
请求值。不同内容复杂度之间不得直接比较 PSNR；山景素材明显比航拍更容易编码。

驱动默认 `time_delta_based_rc=1` 与 GStreamer 1.28.2 的 frame-number 时间戳
约定不兼容：诊断中，VBR 请求 4/8/16 Mbps 均产生约 54.5 Mbps，CBR 请求
8 Mbps 产生约 117.5 Mbps。这组数据只证明默认路径异常，不得进入 FPGA 码控
GOLDEN。正式对标统一使用 `time_delta_rc=off`。

当前 1080p60 保存码流的主要语法特征为 Main 8-bit 4:2:0、CTU 32x32、变换块
4x4 至 32x32、3 列 x 1 行 tile、SAO 和 TMVP 开启、WPP 关闭。完整字段见
`/data/qcom_sdk/artifacts/hevc-full/real-syntax.json`。这是 Q6A 输出样例，不要求 FPGA 复制相同
分块或工具决策，除非项目接口规范另行锁定这些语法。

## 4. 测试资源目录

视频实体统一存于 `/video`，不保留 QCOM 素材旧路径软链接。当前可用性与认证状态以
`/video/metadata/baseline.json` 为准；缺失条目不能以新素材替代旧 GOLDEN。
旧路径映射保存在 `/video/metadata/migration.json`，历史报告与封存证据不重写。

以下测试素材、码流、报告和汇总均使用当前工作站绝对路径。换机或归档后必须
整体重定位，并以 manifest SHA256 而不是路径字符串判断资源身份。

### 4.1 原始输入素材

| 路径 | 内容 | 关键身份 |
|---|---|---|
| `/video/raw/baseline/drone-1080p60.nv12` | 3600 帧连续航拍，1920x1080@60，11,197,440,000 字节 | SHA256 `05f9dd25c4da1d5c4931cc593c439ee37d7896efe8e9f118c4e32e31ca7a55c6` |
| `/video/legacy/qcom/hevc-full/drone-1080p60.nv12.zst` | 上述原始帧的单帧 Zstandard 无损压缩文件 | 解压后必须得到上一行 SHA256 |
| `/video/legacy/qcom/hevc-full/drone-2160p30.nv12` | 原始 4K60 航拍取偶数序号帧，3840x2160@30，1800 帧 | SHA256 `5fcffbbf6d0233265396cfe3565a4f179552b4448449fdbf5d872e3086bc4382` |
| `/video/legacy/qcom/hevc-full/drone-2160p30.nv12.zst` | 上述 4K30 原始帧的无损压缩文件 | SHA256 `1d5f659d5aa63c474fdc94fa0feb9c94824b0e80765c50291792b08593105db0` |
| `/video/raw/baseline/swiss-mountains-65s.nv12` | 1920x1080@30，1950 帧山景 | SHA256 `54cee0fc7c0cfa1f0a55af1a4f17fee06278b5d19a42a704b2f043377cbd6836` |

来源、许可、转换方法和原始媒体 SHA256 分别记录在：

- `/video/legacy/qcom/hevc-full/corpus-1080/manifest.json`（保留航拍来源与转换参数）；
- `/video/legacy/qcom/hevc-full/drone-2160p30-manifest.json`；
- `/video/metadata/provenance/swiss_historical.json`（恢复的历史来源记录）。

航拍原片为 60000/1001 fps。测试按 60 fps 重放，相当于 1.001 倍速，不插帧、
不补帧；4K30 输入选取原片序号 0、2、...、3598，不做空间缩放。

### 4.2 固定解码输入

`/video/legacy/qcom/hevc-full/real-decode/` 是 FPGA 解码器首选的真实场景候选向量：

| 文件 | 规格 | 字节数 | SHA256 |
|---|---|---:|---|
| `/video/legacy/qcom/hevc-full/real-decode/drone-1080p60-b.h265` | Main 8-bit，1920x1080@60，3600 帧，含 B 帧 | 61,038,402 | `926465cb672cd28584fac76cdcd73ba48c7e60b55b3e40bc1adcc7378099b2b2` |
| `/video/legacy/qcom/hevc-full/real-decode/drone-2160p60-b.h265` | Main 8-bit，3840x2160@60，3600 帧，含 B 帧 | 91,488,553 | `028db6fe5d04c7889d8aac3d59af03707e1e6a062c7cb4efbd48c570e9a331d4` |

每个 `.h265` 必须与同名 `.h265.index` 和 `.h265.probe.json` 一起使用：

- `.index` 每行是 `byte_offset byte_size presentation_index`，按解码顺序排列；
- `presentation_index` 是 0..N-1 的排列，用于恢复含 B 帧码流的显示顺序；
- `.probe.json` 保存 FFprobe 解析出的流和逐帧信息；
- `manifest.json` 保存所有文件的字节数、SHA256、生成器版本和编码参数。

这两组码流由 host libx265 `fast` preset 生成，固定 GOP 60、关闭 scenecut、重复
参数集。它们适合验证 FPGA Annex B 解析、参数集、I/P/B 帧、重排序、EOS 和
连续吞吐，不代表 HEVC 全标准覆盖。

### 4.3 Q6A 输出、报告与汇总

| 目录 | 用途 |
|---|---|
| `/data/qcom_sdk/artifacts/hevc-full/delivery/` | 每次板测的 `trial.json`、`report.json`、控制读回、逐秒采样、逐帧时序、保存码流和画质结果 |
| `/data/qcom_sdk/artifacts/hevc-full/summary/runs.csv` | 每个会话的一行性能摘要 |
| `/data/qcom_sdk/artifacts/hevc-full/summary/summary.json` | 当前单路交付的完整机器可读汇总 |
| `/data/qcom_sdk/artifacts/hevc-full/summary/quality.csv` | 同源完整画质摘要 |
| `/data/qcom_sdk/artifacts/hevc-full/summary/real-capacity.png` | 单路无节流容量图 |
| `/data/qcom_sdk/artifacts/hevc-full/summary/real60-timeline.png` | 1080p60 的 100 ms 带宽和逐帧延迟时序 |
| `/data/qcom_sdk/artifacts/hevc-full/audit.json` | 报告哈希、输入哈希、控制读回、帧计数和系统设置恢复审计 |
| `/data/qcom_sdk/artifacts/hevc-full/restore-audit.json` | 最终 CPU governor、VPU 投票和残留测试进程检查 |

`/data/qcom_sdk/artifacts/hevc-full/delivery/real-quality-rc/` 和
`/data/qcom_sdk/artifacts/hevc-full/delivery/real-rc-probe/` 中显式关闭 time-delta RC
的保存码流及 `quality/quality.json` 可作为编码器比较样例。旧目录
`/data/qcom_sdk/artifacts/hevc-full/delivery/real-first/`、
`/data/qcom_sdk/artifacts/hevc-full/delivery/real60-first/`、
`/data/qcom_sdk/artifacts/hevc-full/delivery/real-rate/` 使用旧输入或默认
RC 路径，只能用于故障诊断。

建议优先使用以下保存码流作为 Q6A 编码参考样例；表中 SHA256 是码流本身，
对应目录下的 `quality/quality.json` 保存完整逐帧画质结果：

| 配置 | 保存码流绝对路径 | 码流 SHA256 |
|---|---|---|
| 1080p60 VBR 4 Mbps | `/video/legacy/qcom/hevc-full/delivery/real-quality-rc/custom/fbe7e6db150c20af/session-0-1789893309623954448/loopback-1789893309648101-5796/output.h265` | `c7b388fc602a347a45a75df3488f3a98a70481378789ad69ac4d98d030f56171` |
| 1080p60 VBR 8 Mbps | `/video/legacy/qcom/hevc-full/delivery/real-rc-probe/custom/3907050ae7ddbaa9/session-0-1789893049153757412/loopback-1789893049178437-5595/output.h265` | `be224fe92cb599e645868b969287e3dff1d32a35f269d7dc8baec17c803a0d01` |
| 1080p60 VBR 16 Mbps | `/video/legacy/qcom/hevc-full/delivery/real-quality-rc/custom/ea70f46f698e71fd/session-0-1789893419205197375/loopback-1789893419229629-5820/output.h265` | `928176593e94b3266f8f8bfcab6ec6f2b1576b835db8fc8d97895931e83cf467` |
| 1080p60 CBR 8 Mbps | `/video/legacy/qcom/hevc-full/delivery/real-rc-probe/custom/f77540d2b6209ce3/session-0-1789893159025132422/loopback-1789893159049596-5632/output.h265` | `d7ad5e733f6a0e99557efaeb0a3f0453e096929efe77c65328009570ff42e732` |
| 4K30 VBR 16 Mbps | `/video/legacy/qcom/hevc-full/delivery/real-4k-bounded/custom/6afc9e0e23c2498a/session-0-1789896069521722926/loopback-1789896069546473-6735/output.h265` | `da51bec1172e11bbc1bfd8e41230bef1f0aecf5b0d432e19c6544611841683d1` |

`/data/qcom_sdk/artifacts/hevc-full/excluded-by-scope/` 保存用户明确排除的双路原始记录。该目录
不得进入单路 GOLDEN、性能汇总或验收结论。

## 5. GOLDEN 分层

### 5.1 输入 GOLDEN

输入 GOLDEN 冻结“送入 DUT 的比特”，不冻结文件名。每条向量至少登记：

- 向量 ID 和用途；
- 宽、高、帧率、像素格式、色域/范围和帧数；
- 原始字节数和 SHA256；
- HEVC 码流的 AU 索引、显示顺序和解析结果；
- 来源、许可、转换命令和工具版本。

首批建议冻结三条原始输入和两条固定 HEVC 输入：1080p60 航拍、4K30 航拍、
1080p30 山景，以及 1080p60/4K60 B 帧码流。CI 应先校验 SHA256，再启动仿真
或板测；身份不匹配时直接失败。

### 5.2 解码 GOLDEN

固定 `.h265` 及其 AU 索引只是解码输入向量，还不是完整的像素 GOLDEN。正式
晋升时应使用版本锁定的 HEVC 参考解码器生成可见区域的 NV12，并由第二个独立
软件解码器交叉验证。两者逐帧 Y/U/V 必须字节一致，然后保存：

- 完整输出 SHA256；
- 每帧 Y、U、V 平面 SHA256；
- coded size、visible crop、bit depth、chroma format 和显示序号；
- 参考解码器名称、版本和完整命令。

FPGA 解码器按 `presentation_index` 与参考帧比较，Main 8-bit 输出要求可见区域
逐字节一致，并同时检查输出帧数、顺序、EOS、错误标志和 backpressure 行为。
不要把 Q6A 解码输出作为唯一像素真值；Q6A 结果在这里用于性能交叉检查。

当前资源包尚未保存上述两组固定码流的独立参考解码 NV12 和逐帧哈希。因此它们
目前是“已锁定输入候选”，完成双参考解码和哈希清单后才可标记为解码 GOLDEN。

### 5.3 编码 GOLDEN

合法 HEVC 编码通常不唯一。运动搜索、模式决策、QP 分配、tile 和环路滤波选择
不同，都可能产生语义正确但字节不同的码流，所以不得要求 FPGA 与 Q6A
`output.h265` 逐字节相等。编码验收应同时检查：

1. 码流可由两个独立软件解码器完整解码；
2. 输入、编码和解码帧数相等，无重复、遗漏、时间戳倒退或无效显示序号；
3. SPS/PPS、可见尺寸、bit depth、chroma format 和项目锁定的语法约束正确；
4. 在相同源、RC 模式和实际码率下，逐平面 PSNR/SSIM 达到项目阈值；
5. 100 ms 和 1 s 窗口的码流突发、平均码率和最大 AU 满足输出缓冲预算；
6. 实时吞吐达到功能实时线，容量测试再与 Q6A 对标线比较。

本文第 3 节数值是初始画质锚点，不宜直接变成严格门限。建立 FPGA 自身
GOLDEN 时，应先冻结算法版本，在完全相同的输入和实际码率下生成软件模型结果，
再按产品容差确定质量下限。任何语法或码控算法变化都必须生成新的 GOLDEN 版本，
不能静默覆盖旧结果。

## 6. FPGA 数据通路预算

NV12 可见图像的单向裸帧流量为：

| 规格 | 字节/秒 |
|---|---:|
| 1080p30 | 93.312 MB/s |
| 1080p60 | 186.624 MB/s |
| 4K30 | 373.248 MB/s |
| 4K60 | 746.496 MB/s |

这些数值仅为 `width * height * 1.5 * fps`，不包含参考帧回读、重建写回、运动
搜索、中间结果、突发低效和 AXI 仲裁，不是 Q6A DDR 实测带宽。

若以 32x32 CTU 调度，1080p60 为 122,400 CTU/s，4K30 为 244,800 CTU/s。
仅作 200 MHz 调度预算时，两者分别约有 1,634 和 817 cycles/CTU。该换算不能
推导 Q6A 内部周期，也不能替代 FPGA 综合后的 LUT/BRAM/DSP、时序和 DDR 评估。

## 7. 建议的首批 FPGA 回归

按以下顺序建立证据，每个阶段失败时保留输入、输出和第一处差异：

1. 使用小型合成语料验证 Annex B、参数集、AU 边界、I/P/B 重排序和 EOS；
2. 将 `drone-1080p60-b.h265` 纳入解码逐帧像素 GOLDEN；
3. 将 `drone-2160p60-b.h265` 纳入 4K60 解码逐帧像素 GOLDEN；
4. 使用 1080p60 航拍依次验证 4/8/16 Mbps 编码码控、画质和 60 fps 实时线；
5. 使用 4K30 航拍验证 16 Mbps 编码、45.9 fps Q6A 对标线和完整 1800 帧质量；
6. 编解码核共享存储系统后，单独验证 4K30 回环，不沿用独立核容量结论。

每次回归报告至少保存输入和二进制 SHA256、RTL/软件版本、编解码配置、帧计数、
吞吐、首帧/稳态/排空延迟、错误计数、码率窗口、最大 AU、画质和资源/时序结果。

## 8. 已知边界

- 当前正式结论只覆盖单路 HEVC Main 8-bit 4:2:0；Main10 原生输出未验证。
- 未执行多小时长稳，不提供零丢帧的长期保证。
- 没有有效的板端功耗、能量或 DDR 流量计数，不能从本文计算能效。
- Q6A CPU、RSS 和温度是系统级观察值，不是 VPU 硬件资源利用率。
- 4K 编码和独立解码的容量测试为 15 秒短窗口；1080p60 编码容量窗口为 55 秒。
- 通过 API 枚举得到的最大尺寸不是实际性能通过证明。
- 测试没有刷机、重启或替换桌面应用；主机测试、Yocto 包 QA 与板端性能证据是
  不同验证层。

在这些边界内，当前资源足以建立可复现的输入向量、单路吞吐参考和编码质量
锚点。解码像素 GOLDEN 仍需补齐版本锁定的独立参考输出及逐帧哈希。
