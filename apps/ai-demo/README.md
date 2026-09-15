# Q6A AI Demo

在 Radxa Dragon Q6A 的现有 Yocto 系统上运行离线目标检测。C++17、GTK3、
GStreamer GUI/CLI 共用 QNN HTP 推理核心。网络仅在 NPU 上运行；设备、模型或
运行库不可用时返回失败，不进行 CPU/GPU 推理回退。

## 功能

- JPEG/PNG 图片、H.264/H.265 视频文件，显示 COCO 80 类检测框与置信度。
- YOLOv8n 640×640，8 位权重 / 16 位激活；letterbox、RGB/255 输入和 class-aware NMS。
- GUI：内置图片、文件选择、开始/停止、置信度、推理耗时和实际处理 FPS。
- 视频使用 V4L2 硬件解码，明确协商线性 NV12；不把 UBWC 压缩缓冲当作普通像素。
- 最多缓存两帧；显示线程只保留最新结果。网络推理在线程中执行，GUI 线程只绘制。
- JSON 包含状态、最后一帧检测结果、加载时间、p50/p95、FPS、峰值 RSS 和 QNN profiling。
  `inference_*` 包含 I/O 量化、QNN 调用与反量化；DSP 核心时间单列在 `qnn_profile` 中。

第一版不包含摄像头、模型切换、训练、系统镜像或 OTA。演示 MP4 是公交车样图的
10 秒平移缩放动画，用于检查视频链路，不作为真实动态场景精度数据集。

## 构建

在仓库根目录：

```sh
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app validate --app ai-demo --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app ai-demo --machine radxa-dragon-q6a
```

不要调用 `image` 或 `all`。应用 manifest 保留框架要求的 image 元数据，但本交付只构建应用。
模型作为独立可校验资产打包，不放入 Git 或通用源码包中。

使用可用的 Yocto CMake toolchain 也可直接交叉编译：

```sh
cmake -S apps/ai-demo -B artifacts/ai-demo/arm64 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/yocto/toolchain.cmake
cmake --build artifacts/ai-demo/arm64 -j4
```

依赖 GTK3、GStreamer core/app/video/pbutils 头文件与 QNN SDK 头文件；使用 SDK 配套
编译器、sysroot 和 pkg-config 环境。主机测试通过 `ctest --test-dir <build> --output-on-failure`。
`--self-test` 不调用 QNN，适合 QEMU；不能用它证明 NPU 可用。

## 模型准备

使用 QAIRT **2.47.0.260601**，解包保留 `bin`、`lib`、`include`、`share`；执行文件须有执行权限。
转换器使用 Python 3.12，依赖 numpy 1.26.4、onnx 1.17.0、onnxruntime 1.20.1、
setuptools <81、pyyaml、packaging、pandas、scipy、mako、lxml 和 opencv-python-headless。
还需主机 libc++.so.1、libc++abi.so.1、libunwind；可放在独立目录，无需修改系统库。

导出环境使用 Python 3.11、CPU 版 torch 2.14.0、torchvision 0.29.0、ultralytics 8.3.0、
onnx 1.17.0。脚本显式选用传统 ONNX 导出器，输入/输出尺寸固定，opset 17。
完整实际依赖版本写入生成的 `model-manifest.json`。

```sh
python3 -B apps/ai-demo/scripts/prepare-model.py \
  --sdk /path/to/qairt/2.47.0.260601 \
  --output artifacts/ai-demo/model \
  --export-python /path/to/export-venv/bin/python \
  --converter-python /path/to/converter-venv/bin/python \
  --host-library-path /path/to/host/lib
```

脚本验证固定 SHA-256 的官方权重、样图与 COCO128 压缩包，取排序后的前 32 张图校准，
bus.jpg 不参与校准。HTP context 使用 SoC 35、Hexagon v68、2 MB VTCM。
同名 `model/yolov8n.bin` 是转换器权重归档；部署时必须使用 **`model/context/yolov8n.bin`**。

模型与样图来自 [Ultralytics v8.3.0](https://github.com/ultralytics/ultralytics/tree/v8.3.0)，
采用 AGPL-3.0；打包需把该版本的 LICENSE 保存为 `model/LICENSE-model`。
应用原创代码为 MIT；QAIRT 和 DSP 运行库保留各自 Qualcomm 许可证，不重新声明为 MIT。

## 独立应用包

```sh
python3 -B apps/ai-demo/scripts/package.py \
  --build artifacts/ai-demo/arm64 --model artifacts/ai-demo/model \
  --dsp tmp/sysroots-components/all/hexagon-dsp-binaries/usr/share/qcom/qcs6490/radxa/dragon-q6a/dsp/cdsp \
  --output artifacts/ai-demo/q6a-ai-demo-1.0.0 --version 1.0.0
```

包内含应用、模型、样图、可选 demo.mp4、DSP shell、DSP C++ 库、许可证和 SHA256SUMS。
只从与板上 CDSP 固件匹配的 Q6A sysroot 获取 DSP 文件。
在板端可写目录解包，以 root 运行 `./install`。安装器校验文件，将版本放入
`/var/lib/q6a-ai-demo/releases/` 并切换 `current`，创建 `q6a-ai-demo` 命令和桌面入口。
为了让桌面用户访问 NPU，必要时将 weston 加入 fastrpc 组；桌面入口使用 `sg`，
不需要重启 Weston。安装不更换系统运行库、内核、固件或 OSTree 部署。

FastRPC 1.0.7 的 `open_shell()` 不读取 `ADSP_LIBRARY_PATH`，因此启动器传入私有
`--dsp-dir`。程序先把用户输入/输出路径变成绝对路径，再切换工作目录；DSP shell
与依赖通过应用目录读取。无需修改 cdsprpcd 服务配置。

```sh
# CLI（root）
q6a-ai-demo --input /var/lib/q6a-ai-demo/current/share/ai-demo/data/bus.jpg \
  --report /var/lib/q6a-ai-demo/image.json --snapshot /var/lib/q6a-ai-demo/image.png

# GUI，SSH 启动时指定板上实际显示会话
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 GDK_BACKEND=wayland \
  q6a-ai-demo --gui

# 10 分钟循环视频检测；时限结束写报告，GUI 保留最后结果
q6a-ai-demo --input /path/to/video.mp4 --duration 600 --report /var/lib/q6a-ai-demo/video.json

# 单图重复测量；默认不丢弃首帧
q6a-ai-demo --input /path/to/image.jpg --repeat 100 --report /var/lib/q6a-ai-demo/repeat.json
```

`--confidence` 范围 (0,1)，默认 0.25，NMS IoU 为 0.45。视频未设置 `--duration`
时运行到 EOF；设置时循环到时限。SIGINT/SIGTERM 结束 CLI 并报告 CANCELLED。
`--dump-input` 保存首帧 NHWC float32 输入，`--dump-output` 保存首帧反量化输出，
用于与 `qnn-net-run` 对照。报告保存最后一帧，故单图对照应使用默认 `--repeat 1`。

卸载/撤回：运行当前版本的 `uninstall`。存在上一版本时恢复；首次安装则移除入口和
本安装器添加的 weston 组成员资格。版本文件与结果保留，便于复核。

## 验收

1. 主机 CTest、manifest 检查、ARM64 构建和 QEMU 自检。
2. 实板图片应识别公交车与至少三名行人；同量化模型、相同输入与官方 qnn-net-run 比较输出。
3. QNN profiling 必须包含有效 accelerator execute 事件；仅显示后端名称不算硬件证据。
4. H.264/H.265 文件、损坏输入、缺失模型、停止/重启、文件切换和置信度调整。
5. HDMI GUI 连续运行 10 分钟，记录 RSS、截图、处理帧数、FPS、推理 p50/p95。

构建、离线测试和实板结果分别记录在 `artifacts/ai-demo/`。不预设未经测量的 FPS 承诺。
