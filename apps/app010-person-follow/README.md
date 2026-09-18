# Q6A 人物运动跟踪 / Person Follow

独立 C++17 / GTK3 应用：QNN HTP 人物检测、ByteTrack 多人 ID 和轨迹、
点击锁定单人及画面中心偏差。GUI 为中文，同时提供 CLI。仅 NPU 推理，
缺少模型或 HTP 时明确失败；CPU 承担解码衔接、预处理、关联和渲染。
本版使用离线 H.264/H.265 公共视频，不接摄像头或物理云台。

## 操作与输出

打开视频或示例视频，点击人物框锁定（重叠框选择面积较小者），可切换人物
或取消锁定。黄色框和连线表示有效目标；显示阈值不隐藏已锁定目标。
短时遮挡保留 ID，不显示预测目标位置；按视频时间超过 1 秒后标记丢失，
必须重新选择。相同 ID 再次出现也不会自动恢复已超时的目标。
更换文件、重播、循环均重置锁定和轨迹。无外观重识别，ByteTrack 的 ID
关联仍可能出错；界面锁定不是跨遮挡、跨视频身份识别保证。

```sh
q6a-person-follow --gui
q6a-person-follow --input video.mp4 --target-id 1 --tracks tracks.jsonl --report report.json
q6a-person-follow --gui --input video.mp4 --duration 600 --report soak.json
```

独立安装包提供 `q6a-person-follow`；Yocto recipe 安装 `person-follow`。
CLI 未指定 ID 时展示多人；指定 ID 后等待它出现。CLI 默认完整处理各帧，
GUI 按播放时钟运行并丢弃过期帧，`--exact` 改为逐帧处理。
`--duration` 指处理阶段的墙钟秒数，可循环视频；循环时不重新应用 CLI ID。
`--batch` 为离线图片评估入口，不产生时序 ID。

逐帧 JSONL 保留 `source_frame`、`pts_seconds`、`loop`、尺寸、人物框、关键点
及 ID，并新增 `target: {id, state, offset}`。`state` 为 `unselected`、
`waiting`、`tracking`、`occluded` 或 `lost`。`offset` 是相对中心归一化
`[x,y]`：左/上为负，右/下为正，范围 [-1,1]；无有效目标为 `null`。
报告包括最终目标状态、模型 SHA256、HTP 执行事件、FPS、丢帧和时延。
报告 `PASS` 只表示运行完成，不表示精度或物理云台验收通过。

## 构建与安装

```sh
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app validate --app app010-person-follow --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app app010-person-follow --machine radxa-dragon-q6a
```

依赖 GTK3、GStreamer app/video/pbutils、QNN 头文件和 Eigen3。
主机配置 CMake 后执行 `ctest --test-dir <build> --output-on-failure`。
`tests/test_contract.py --probe <build>/pose-contract-probe --imgsz 640`
验证 Python/C++ 预处理及解码契约，`tests/test_data.py` 验证标注转换。

`scripts/package.py --build <arm64-build> --model <selected-model> --selection
<selection.json> --assets <assets> --dsp <cdsp-dir> --version <version> --output
<bundle>` 创建自包含版本包。模型清单、冻结选择和哈希必须一致；允许评估后
选择预训练基线，但不会标记为已微调。资产目录含 sample.png、sample.mp4、
sources.json。仅使用转换后的 `context/pose.bin`，不用中间同名文件。
在板端执行包内 `install`，安装到 `/var/lib/q6a-person-follow/releases`，
原子更新 current；`uninstall` 回退上一版或移除本应用入口，保留证据。
不重建镜像、不 OTA、不刷写或重启。

## 训练与模型选择

基于 app008 的实现独立建立，保留 ByteTrack 的许可证与来源。
Python 3.11 环境依赖见 `scripts/requirements-train.txt`；模型原始权重和
工具保持其 AGPL-3.0 许可。GPU 必须通过 `scripts/check-gpu.py` 的实际
前向、反向及优化步骤测试。准备 COCO 数据采用固定 seed 42，训练 5000 张、
验证 500 张、最终测试 500 张；非 crowd 且无可见关键点的人物框仍保留。
测试集只保证未参与本次微调，不保证从未参与原模型预训练。

```sh
python -B scripts/train.py --data DATA --output TRAINED --weights yolov8n-pose.pt \
  --conservative --lr 0.000005 --imgsz 640 --epochs 4 --train-only
python -B scripts/select-model.py --data DATA --weights yolov8n-pose.pt \
  --candidate TRAINED --videos videos.json --build HOST_BUILD --output EVALUATION
python -B scripts/convert.py --sdk QAIRT_2_47 --output EVALUATION/selected \
  --converter-python CONVERTER_PYTHON --host-library-path LLVM_LIBRARY_DIR
```

脚本路径相对于本应用目录。训练保留完整网络结构，冻结 backbone/neck 和
BatchNorm 统计，禁用强增强，batch 4、名义累积 batch 16、AdamW；显存不足
在新输出目录降到 batch 2、1，不自动 CPU 回退。权重须匹配固定官方 SHA256，
旁边保留 LICENSE-model。

视频来自 https://motchallenge.net/data/MOT17/，使用官方 raw 预览视频及手工
GT，不使用检测结果覆盖视频。保留下载来源、许可范围、转码参数及哈希；公开
可下载不代表无限制商用。`videos.json` 为列表，每项含 `sequence`（02/10/05/09）、
`split`（val/test）、`video`、`gt`、`frames`、`scale`；路径相对执行目录。
验证序列为 02/10，测试为 05/09，各序列只使用一份。

候选模型与基线使用相同 640 输入及 ByteTrack 参数。验证序列 IDF1 按 GT
观测数加权，严格提升且总召回率不下降才选择候选，否则采用基线。
先写 selection.json 冻结选择，再进行最终测试。报告 bbox/pose AP、IDF1、
ID switches、漏检、误检，以及生产 C++ 目标锁定逻辑的正确率、覆盖率、错误
关联和短时恢复次数。锁定评估从每个 GT 人物首次匹配开始，不自动重选，
不是实测鼠标操作或 NPU 结果。`video-reference.py` 只用于 CUDA 浮点评估，
不是应用的推理后备路径。

ONNX bbox AP 降幅须 <=0.5 个百分点；NPU 相对选定浮点模型 <=2 个百分点。
量化使用 QAIRT 2.47.0.260601，SoC 35、HTP v68、W8A16、逐通道权重、
32 位 bias，校准仅取训练集固定 128 张。

## 验收边界

主机测试覆盖选择、超时、禁止自动换人、坐标映射、重播重置、跟踪及预处理。
板端须验证 HTP 执行、完整 H.264/H.265 视频、GUI 选择/取消/停止/重播/切换、
正常退出、缺失资源错误，以及 600 秒连续运行。性能按实测，不承诺实时帧率。
模型、原始视频、训练日志及验收报告放在仓库外部生成区
`artifacts/person-follow/`，不提交到源码。
