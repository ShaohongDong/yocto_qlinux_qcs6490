# Q6A Wi-Fi 测试

中文 GTK3 桌面与命令行共享 libnm 后端，支持扫描、个人网络连接、状态、断开和绑定无线接口的 Ping。
通过 NetworkManager 管理网络，不启动独立 supplicant 或 DHCP 客户端。

## 构建

在 SDK 根目录的新 shell 中执行：

```sh
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app validate --app app006-wifi-test --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app app006-wifi-test --machine radxa-dragon-q6a
```

可选镜像命令为 `scripts/qcom-app image --app app006-wifi-test --machine radxa-dragon-q6a`，
默认采用多媒体 SD 镜像。构建命令不会烧录介质。
主机安装 GTK3/libnm 开发包后可用 CMake 构建，并执行 `ctest --test-dir <build> --output-on-failure`。
`wifi-test --self-test` 不访问 D-Bus、无线网卡或显示设备，可用于 QEMU 用户模式检查。

## 桌面

点击 Weston 的 Wi-Fi 图标，或在桌面终端运行 `wifi-test --gui`。
选择网卡、扫描热点、选中网络并输入密码。隐藏网络需手动输入 SSID，并明确选择 open/wpa2/wpa3。
支持开放网络、WPA2-PSK、WPA3-SAE；企业认证、WEP、OWE 和网页登录自动化不在范围内。
扫描显示每个 BSSID，使用 SSID 原始字节匹配热点；JSON 同时提供 `ssid_hex`。
非 UTF-8 SSID 仅可显示替代字符，当前 GUI 不提供原始字节输入。

默认配置仅在内存中存在，断开后由 NetworkManager 删除，关闭窗口不会断开已成功建立的连接。
勾选“记住此网络”时，先完成认证和地址配置，成功后才保存到 NetworkManager 并启用自动重连。
应用创建独立 UUID 配置，不覆盖现有保存的网络；重复保存会生成多个配置，可用系统网络工具清理。
普通桌面用户创建的配置仅授权该用户，使用系统的 `settings.modify.own` 权限；root 创建系统配置。
密码不接受命令行参数，不进入日志或 JSON；持久密码由 NetworkManager 按系统权限保存。

Ping 留空使用当前无线网关，可输入 IP 或域名；默认 4 次，操作总超时 60 秒。
使用 iputils 的 `ping -I <无线接口>`，避免从有线接口得到假阳性的连通结果。
优先使用程序实际目录下的 `ping.iputils`（只读系统的便携部署），否则使用 `/usr/bin/ping.iputils`。
域名解析使用系统 DNS；ICMP 数据流绑定所选无线接口。界面显示实时输出、丢包及往返时延。
“停止”会取消当前操作；窗口关闭时先清理进行中的任务。

## 命令行

```sh
wifi-test scan --interface wlan0 --json
wifi-test connect --interface wlan0 --ssid 'Test Network' --security wpa2
wifi-test connect --interface wlan0 --ssid 'Hidden Network' --hidden --security wpa3 --remember
wifi-test status --interface wlan0 --json
wifi-test ping --interface wlan0 --count 4 --json
wifi-test ping --interface wlan0 --target example.org --count 10 --timeout 30 --json
wifi-test disconnect --interface wlan0 --json
```

交互连接隐藏密码回显。自动化通过 `--password-stdin` 从受保护的标准输入读取一行，
不要把密码写入 shell 历史、命令参数或测试报告。开放网络使用 `--security open`。
多个无线网卡时必须指定 `--interface`。退出码：0 成功、1 运行失败/取消/超时、2 参数错误。
`--json` 输出一个对象，包含 `ok`、`command`、`message`、接口、地址、热点及 Ping 结果；
无统计数据时 `ping` 为 null，无有效回复时 `rtt_ms` 为 null。

## 现有系统部署与验收

先通过有线 SSH 或串口核实 Q6A 身份、NetworkManager 版本、wlan0 管理状态与桌面会话。
需要 GTK3、libnm、NetworkManager Wi-Fi 插件、wpa-supplicant、iputils-ping 和
`ttf-wqy-zenhei` 中文字体。字体是应用的显式运行依赖，防止中文显示为方框。
安装程序、desktop、SVG 和 Weston PNG 到对应标准目录，在现有 weston.ini 追加应用 launcher。
保留被替换文件和原配置，以便回退。桌面启动入口必须来自实际的活动本地会话；SSH 中
`runuser -u weston` 不等于活动桌面会话，Polkit 可能拒绝授权。应用不安装宽松授权规则。
NetworkManager 未管理无线接口时应用明确报错；不要强行接管正在承担管理连接的接口。
Q6A 多媒体镜像的 URM 默认会将桌面进程移出 logind 会话，导致标准 Polkit 授权失败。
选择本应用时，URM 配置会排除完整启动链 `weston-start.sh`、`weston`、
`weston-desktop-`（Linux comm 的 15 字节名称）及 `wifi-test`，保留登录会话。
仅排除应用和桌面 shell 不够：启动脚本被分类后，子进程可能继承错误的 cgroup。
其他应用仍按原 URM 策略管理。
现有系统部署需备份并追加这四个名称到 `/etc/urm/classifier/classifier-blocklist.txt`，
依次重启 URM 和 Weston，使新进程继承正确会话；不需要新增 Polkit 授权规则。

对于 `/usr` 只读的 OSTree 系统，将应用包与 iputils-ping 包解包到
`/var/opt/wifi-test/releases/<源码摘要>/`，保留 `usr/bin` 和 `usr/share` 目录结构，
以 `/var/opt/wifi-test/current` 指向当前版本。Weston launcher 使用
`/var/opt/wifi-test/current/usr/bin/wifi-test --gui`，图标使用
`/var/opt/wifi-test/current/usr/share/weston/wifi-test.png`。
在 `~/.local/share/applications/` 安装使用相同绝对路径的 desktop 文件。
该方式不修改 `/usr` 的只读挂载；回退时恢复 Weston 配置和 current 链接。
保留 iputils 包的网络权限，或核实 `net.ipv4.ping_group_range` 允许桌面用户使用 ICMP socket。
便携部署同时将文泉驿字体及其许可证安装到桌面用户的 `~/.local/share/fonts/wifi-test/`，
运行该用户的 `fc-cache -f` 后重新启动应用。

验收记录应包含桌面图标启动、扫描、认证、取得地址、无线网关 Ping、域名 Ping、取消、
错误密码、断开重连以及持久/临时配置的重启行为。记录必须脱敏。
主机测试、交叉编译和 QEMU 自测不代表实板 Wi-Fi 或桌面验收通过。

NetworkManager API: https://www.networkmanager.dev/docs/libnm/latest/NMClient.html
