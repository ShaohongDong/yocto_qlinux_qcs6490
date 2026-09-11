# Qualcomm 默认 OTA

`qcom-distro` 及其派生发行版默认使用 OSTree。当前层所有 Qualcomm MACHINE
的 `qcom-*-image` 正式系统镜像生成 OTA 文件系统；测试、救援和 ESP 辅助镜像
不作为升级目标。现有 `qcom-distro-sota` 名称继续有效。

默认没有服务器地址或信任密钥，不轮询、不自动安装、不自动重启。未配置的设备
仍可启动和运行。Aktualizr 云端注册和自动上传不在本方案中。

| 启动类型 | 默认行为 |
| --- | --- |
| 普通 ARM64 EFI 板型（含 Q8B） | systemd-boot，版本化 UKI，3 次启动尝试，健康确认后保留上一版本 |
| Dragon Q6A | 保留 embloader，版本化原始内核/initramfs/DTB；BLS 计数与失败回退 |
| qcom-armv7a | 固定 Android 启动分区；initramfs 读取原子部署链接，仅升级用户空间 |

ARMv7 的每次更新必须保持内核、initramfs、模块、DTB 和固化启动参数的指纹一致，不宣称支持内核升级或
启动失败自动回退。需要变更这些文件时，重新生成并安装完整初始镜像。

## 构建

在 SDK 根目录的全新 shell 中激活环境，设置发布版本后构建：

```sh
source ./environment-setup-armv8a-qcom-linux
# 在构建配置中设置 QCOM_OTA_VERSION = "2.0.1"
devtool build-image qcom-minimal-efi-sd-image
layers/oe-core/bitbake/bin/bitbake qcom-minimal-efi-ufs-4k-image
```

切换板型时，在 `conf/local.conf` 中设置 `MACHINE`，或用 `bitbake -r <machine.conf>`
预先加载机器选择。`-R` 配置在机器配置加载后才执行，不能单独用于切换板型。
例如一次性构建 Q8B：

```sh
printf '%s\n' 'MACHINE = "radxa-dragon-q8b"' > /tmp/q8b-machine.conf
layers/oe-core/bitbake/bin/bitbake -r /tmp/q8b-machine.conf radxa-dragon-q8b-flash-bundle
```

OTA 会改变发行版的文件系统和启动布局，因此启用时不使用原 eSDK 的冻结任务
签名；仍按当前元数据正常复用 sstate。首次切换需要重建受影响的组件，避免把旧
非 OTA 包与新镜像配置混合。显式关闭 OTA 后保留 SDK 原来的签名策略。

配置项：

| 配置 | 默认值/用途 |
| --- | --- |
| `QCOM_OTA_ENABLE` | `1`；显式设 `0` 恢复原普通镜像布局 |
| `QCOM_OTA_VERSION` | `2.0.0`；正式发布必须递增，格式 `MAJOR.MINOR.PATCH` |
| `QCOM_OTA_CHANNEL` | `stable`；不同通道使用独立 ref |
| `QCOM_OTA_MIN_FREE_KIB` | `524288`；安装后保留空间，不含下载和部署需求 |

ref 为 `MACHINE/IMAGE_BASENAME/CHANNEL`，不会把 Q6A 版本装入 Q8B，或把 minimal
版本装入 multimedia。SD 和 UFS 镜像也有各自独立的 ref。镜像大小自动包含完整
下载、部署与运行空间预留；仍受显式 `IMAGE_ROOTFS_MAXSIZE` 限制。

`tmp/deploy/images/<machine>/` 中包含：

- `*.ota-ext4`、`*.ota-esp`：真正的 OSTree 部署文件系统与启动分区。
- `*.wic`、`*.wic.bmap`：首次安装用 SD/UFS 整盘镜像。
- `*.ota.json`：板型、镜像、版本、启动后端、兼容指纹和 commit。
- `*.ostreecommit.tar.xz`、`ostree_repo/`：未签名构建仓库，供显式发布使用。

QDL 首次安装包使用 `ota-ext4` 和 OTA ESP。ARMv7 无 ESP，包中的 `boot.img`
强制使用带 OTA initramfs 的镜像，`boot-images/` 保留所有配置 DTB 的启动镜像。
通用 ARMv7 MACHINE 应设置 `QCOM_DTB_DEFAULT` 为目标 DTB 的文件名（去掉 `.dtb`）；
未设置或为 `multi-dtb` 时沿用 DTB 列表第一项作为默认，选择结果记录在
`boot-images/default-dtb`，首次安装前必须与具体硬件对应。Q6A/Q8B flash bundle 带上 OTA 清单和
仓库归档。SPI NOR、UEFI 和 Qualcomm 底层启动固件不通过设备端 OTA 命令改写。

## 签名发布

在具有 GPG 功能的 OSTree 和 GnuPG 的主机上执行。签名私钥放在仓库外；构建镜像
无需私钥。发布工具不会创建生产密钥、上传文件或改变服务器当前版本指针。

```sh
python3 layers/meta-radxa-dragon/scripts/qcom-ota-release.py \
  --repo tmp/deploy/images/radxa-dragon-q6a/ostree_repo \
  --manifest tmp/deploy/images/radxa-dragon-q6a/qcom-minimal-efi-sd-image-radxa-dragon-q6a.rootfs.ota.json \
  --gnupghome /secure/release-gnupg \
  --key FULL_SIGNING_KEY_FINGERPRINT \
  --output /srv/releases/q6a-2.0.1
```

后续发布加 `--previous /srv/releases/q6a-2.0.0`，检查版本递增及固定内核兼容性。
输出目录必须尚不存在。工具比较清单、commit 元数据与文件系统身份，签名 commit
和 summary，再用新的验证仓库验证确切发布内容。最后原子提交输出目录。

用自己的 HTTP(S) 服务托管输出中的 `repo/`。通过可信的出厂配置或管理通道安装
`release-key.asc`，不能从未验证的升级服务器自动信任其公钥。生产主机私钥不进入
镜像、发布目录或 Git。设备没有绕过签名校验的命令参数。

## 设备端

```sh
qcom-ota configure --url https://updates.example.com/q6a/repo --key /root/release-key.asc
qcom-ota status
qcom-ota check
qcom-ota install
qcom-ota reboot
# 需要回退时：
qcom-ota rollback
qcom-ota reboot
```

`check` 下载并验证摘要和 commit 元数据；`install` 下载确切 checksum，检查兼容性
和空间后部署。除 `status` 外需要 root。输出为 JSON，事务状态保存在
`/var/lib/qcom-ota/state.json`；systemd 日志可用
`journalctl -u qcom-ota-confirm.service` 查看。

已有 pending 部署时拒绝再次安装。普通安装拒绝降级以及同版本不同 commit；显式
回滚只能选择保留的兼容部署。失败下载不会改变启动选择。并发操作由文件锁排斥。

首次镜像作为已确认的初始部署。新 EFI 部署有 3 次启动尝试，失败耗尽后选择旧的
已确认部署。系统服务在到达 multi-user.target、挂载正常、指定服务正常后确认
启动。通过 `/etc/qcom-ota/health.json` 的 `services` 数组指定产品关键服务，
例如 `["camera.service"]`；检查最多等待 120 秒。未指定时仅执行基础系统检查，
不会把外设功能视为已验收，也不依赖网络在线来确认启动。

确认后保留当前和上一成功版本；只管理本客户端创建的 pin，不移除管理员的 pin。
旧 ARMv7 的用户空间回滚需要系统仍能运行管理命令；无法启动时使用恢复环境。

## 数据与首次迁移

已有普通 rootfs 不能直接执行第一次 OTA。先备份业务数据和配置，安装新 WIC/QDL
初始镜像，再恢复数据并配置源和公钥。此流程需要单独安排实物刷写。

`/usr` 和随系统交付的 `/opt` 应用随部署版本变化，业务可写数据放入 `/var`。
`/etc` 使用 OSTree 配置合并，`/var` 在部署间共享。初始镜像保留配方提供的 `/var`
种子数据。系统回滚不会回滚数据库或其他业务数据；应用迁移必须兼容上一版本。

## 验证

本次结果及验收边界见 [OTA-VALIDATION.md](OTA-VALIDATION.md)。

```sh
python3 -m unittest discover -s layers/meta-radxa-dragon/scripts/tests -v
python3 layers/meta-radxa-dragon/scripts/audit-qcom-ota-matrix.py --output /tmp/qcom-ota-matrix.json
python3 layers/meta-radxa-dragon/scripts/audit-radxa-dragon-q6a.py --help
```

真实签名、HTTP 拉取、连续部署和配置保留测试需要 OSTree/GPG：

```sh
QCOM_OTA_INTEGRATION=1 unshare -Ur python3 -m unittest discover \
  -s layers/meta-radxa-dragon/scripts/tests -p test_qcom_ota_integration.py -v
```

通用 UEFI 失败启动计数仿真（需要 QEMU、AArch64 UEFI 固件和 mtools）：

```sh
python3 layers/meta-radxa-dragon/scripts/test-qcom-ota-embloader.py \
  --loader tmp/deploy/images/radxa-dragon-q6a/embloader-0.7.efi \
  --output /tmp/qcom-ota-embloader-test
# 测试 systemd-boot 时加 --backend systemd-boot，并传入对应 EFI 文件。
```

该测试使用故意无效的内核负载，检查三次计数、耗尽后旧入口选择和 EFI 异常，
不替代真实内核启动或实板断电测试。

全发行版层组合下可运行 `qcom_ota.QcomOTAMatrixTests`。仅含 oe-core 的 BSP CI
环境会明确跳过该矩阵，SDK 中的 `audit-qcom-ota-matrix.py` 提供等价元数据检查。
板级审计会比较 WIC 分区与 OTA 原始产物，检查实际部署身份、启动文件及 DTB。

元数据、构建、主机集成、UEFI 仿真和实板验收是不同证据。实板需要逐板完成首次
安装、连续升级、断电/失败启动回退和外设回归，才能声称该板 OTA 已经硬件验收。
