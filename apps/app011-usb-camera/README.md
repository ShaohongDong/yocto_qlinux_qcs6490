# USB 摄像头预览

C++17 / GTK3 / GStreamer 中文桌面应用。自动枚举 USB V4L2 采集设备，
排除元数据节点，优先 MJPEG 1920×1080 30 fps，启动后自动预览。
支持设备和组合模式（格式、分辨率、帧率）选择、刷新、开始/停止、全屏；
Escape 退出全屏。切换设备或模式前先停止。仅列出 MJPEG/YUYV/NV12 的
离散尺寸和离散帧率；没有可用模式时提示检查摄像头及权限。

MJPEG 采用 jpegdec 软件解码。GStreamer 线程负责采集和转换，GTK 非阻塞
读取最新 RGB 帧，按比例显示。状态栏接收 FPS 是应用取到的帧率，显示 FPS
是绘制次数，不代表传感器帧率或端到端延迟。队列满时丢弃旧帧。
拔出、占用、权限错误或 5 秒无画面时停止并提示；重新连接后刷新再开始。
不包含录制、拍照、AI、开机自启动、内核或镜像修改。

```sh
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app validate --app app011-usb-camera --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app app011-usb-camera --machine radxa-dragon-q6a
```

Yocto 安装 `usb-camera` 和桌面入口。独立安装使用 `scripts/package.py
--binary <ARM64 executable> --output <new bundle directory> --version <version>`，
将包复制到板端以 root 执行 `install`，通过 `q6a-usb-camera` 启动。
安装到 `/var/lib/q6a-usb-camera/releases`，`current/uninstall` 回退上一版
或移除入口；保留版本文件。桌面程序使用 weston 用户运行，不修改设备权限。

```sh
usb-camera --self-test
usb-camera --list-devices
usb-camera --headless --duration 60
usb-camera --headless --device /dev/video0 --duration 10
usb-camera --gui --duration 60
```

`--device` 仅用于 headless，显式设备默认测试 MJPEG 1080p30。
`--headless --test-source --duration 2` 用于 GStreamer 模拟帧测试，需要
videotestsrc 插件。普通 GUI 无 duration 限制。自检不依赖摄像头或显示服务。
CMake/CTest 验证自检和参数拒绝；`tests/smoke.sh <binary>` 验证模拟采集及
设备启动失败。物理拔插、图像质量及桌面性能须另行上板验证。
