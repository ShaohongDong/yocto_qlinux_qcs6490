# Q6A 音乐播放器

中文 GTK3 桌面应用，使用 GStreamer 播放本地 MP3、PCM WAV、FLAC 和
Ogg Vorbis 文件。提供播放/暂停、停止、进度跳转、音量、单曲循环和输出设备选择。
首版为单文件播放器，无播放列表、网络音乐、歌词或命令行播放模式。

## 构建

从仓库根目录的新 shell 执行：

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app app012-music-player --machine radxa-dragon-q6a
scripts/qcom-app build --app app012-music-player --machine radxa-dragon-q6a
```

面向 `qcom-multimedia-proprietary-efi-sd-image`，构建命令仅构建应用。
应用依赖包含播放、格式解析、MP3/FLAC/Vorbis 解码、音频转换、重采样、
软件音量、ALSA 和 PulseAudio 输出插件，以及 libpulse 客户端库。系统音频服务、声卡驱动和音频路由由现有镜像提供。

## 使用

从桌面应用菜单打开“音乐播放器”，或运行 `music-player`（亦接受 `--gui`）。
点击“打开音乐文件”，选定文件后点击“播放”。初始音量为 50%，不自动播放。
进度条仅在媒体可跳转时启用。停止后再次播放从头开始，播放结束后也可再次播放。
“单曲循环”默认关闭。

桌面输出通过 PulseAudio 协议连接系统音频服务（支持 PipeWire-Pulse）。
默认输出选择服务当前默认的有效 sink；设备列表只显示非空输出。
播放时固定目标设备，默认输出改变不会将耳机声音自动转到其他接口。
没有有效输出、服务断开、耳机端口不可用或流被转移时停止播放并提示；
恢复服务或连接设备后，设备列表及恢复提示自动更新，手动点击“播放”即可；
不会自动续播。“刷新设备”可重新选择系统默认输出。
PAL 输出可以是虚拟 sink，不按“虚拟”属性一概过滤；Dummy Output/null sink 不可播放。
输出检查不能代替物理听音验收，驱动接受数据但硬件无声仍需板端诊断。
切换输出或刷新会停止播放，保留文件和音量；关闭窗口会释放播放与监控资源。

## 验证

`music-player --self-test` 仅执行适用于 QEMU 用户模式的无硬件检查，
不验证音频插件、实际解码或物理出声。

具备 GTK3/GStreamer 开发环境的主机可执行：

```sh
cmake -S apps/app012-music-player -B /tmp/music-player-build -DBUILD_TESTING=ON
cmake --build /tmp/music-player-build
ctest --test-dir /tmp/music-player-build --output-on-failure
```

`playback-test` 使用真实解码管线和 fakesink，自动检查 WAV 播放、暂停、跳转、
播放结束、循环、文件及输出切换、损坏文件和恢复，以及 50% / 25% / 静音的实际 PCM 采样幅度；可额外传入短 MP3、FLAC、Ogg
文件验证格式解码。测试不访问物理音频设备。

宿主机具备 PipeWire/PipeWire-Pulse 时，`output-reconnect` 使用独立服务实例
验证真实空输出、断连和重新连接，不影响桌面音频服务。

`output-routing` 检查空输出、PAL 虚拟输出、服务断连、设备移除、
流迁移、启动超时及恢复后的路由策略。`output-test --probe` 可在目标桌面会话
检查默认输出；退出码 2 表示当前没有可用默认输出，不代表解码测试失败。

板端验收：打开四种格式的已知正常音乐，检查桌面入口、中文显示、播放控制、
输出切换及关闭后的资源释放，并实际听取系统默认输出的声音。
离线自检、fakesink 和交叉构建均不能替代物理出声验收。

用于测试的网络音乐：[Village Ruins — isaiah658](https://opengameart.org/content/village-ruins)，
作者页面标注 CC0 1.0。可下载该页面的 MP3 和 Ogg 原件；测试样本及来源记录
保存在仓库外的 `~/Music/q6a-app012-test/`，不随应用打包。
该目录的 WAV、FLAC 和短 MP3/Ogg 为原曲截取转码，仅用于格式及控制回归。
