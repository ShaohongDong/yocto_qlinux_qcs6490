# OTA 验证记录

日期：2026-09-11。工作区：Qualcomm Yocto eSDK，默认 MACHINE 为
`radxa-dragon-q6a`。本记录区分配置、软件集成和硬件验收；没有刷写或重启实板。

## 已完成的软件检查

最终源码在隔离用户命名空间中一次运行全部 37 项测试通过，包含 8 项真实
OSTree/GPG/HTTP 集成测试。下表列出分项命令与范围。

| 检查 | 结果 | 范围 |
| --- | --- | --- |
| `python3 -m unittest discover -s layers/meta-radxa-dragon/scripts/tests -v` | 29 项通过，8 项集成测试按默认配置跳过 | 客户端策略、失败路径、固定启动选择及首次安装包、出厂数据与已有符号链接、EFI 文件名大小写、启动空间和 DTB 指纹、健康确认、保留部署、实际 C 计数解析器、原有镜像审计测试 |
| `audit-qcom-ota-matrix.py --output /tmp/qcom-ota-matrix-final.json` | 38/38 通过 | 全部 23 个 MACHINE、SD/UFS、显式关闭、辅助镜像排除、三种启动包隔离、主机端与设备端 OSTree 的 UKI/GPG 支持；检查实际 SoC/DTB，执行展开的提交参数确认 JSON 不被拆分；不等于 23 板全部编译通过 |
| 独立构建目录中 `MACHINE = "qcom-armv7a"`，`bitbake -g qcom-minimal-image` | 通过 | 3,835 个配方解析、完整任务依赖解析无错误；包含 `qcom-ota`、`ostree`、`initramfs-ostree-image`，未执行 ARMv7 编译 |
| `QCOM_OTA_INTEGRATION=1 unshare -Ur python3 -m unittest discover -s layers/meta-radxa-dragon/scripts/tests -p test_qcom_ota_integration.py -v` | 8/8 通过 | SDK OSTree 2026.1、临时测试密钥、HTTP 签名拉取、签名失败拒绝、实际部署与回滚、客户端安装、`/etc` 和 `/var` 保留、构建期拒绝损坏的提交元数据，以及真实主机工具生成 UKI 启动入口 |
| `test-qcom-ota-embloader.py --backend systemd-boot --loader <systemd-bootaa64.efi> --output <目录>` | 通过 | 通用 AArch64 UEFI，3 次失败计数及第 4 次旧入口选择 |
| `test-qcom-ota-embloader.py --loader <embloader-0.7.efi> --output <目录>` | 通过 | 同上；同时检查 EFI/内存断言，使用修正后的 embloader |
| kas-container `ci/yocto-patchreview.sh` | 通过 | 47 个补丁，无缺失/格式错误的 Signed-off-by 或 Upstream-Status |

UEFI 测试使用故意无效的内核文件，验证启动器的持久计数和入口选择，不宣称 Linux
完整启动。集成测试使用隔离 sysroot，不操作主机系统，也不等于实板断电验收。

最终构建的 embloader 已通过 `--sector-size 512 --fat-sector-size 4096` 和
`--sector-size 4096 --unicode-name` 两种四轮测试，覆盖 SD、4 KiB UFS 及 UTF-16
非 ASCII 入口名。SHA-256：
`a625ed36ab32e5b1339d723b190cf9daf603fd927d510f66c75a580b9b4b7363`。
本轮 systemd-boot 也通过 4 KiB 扇区四轮测试，SHA-256：
`45961c79f7b3e70cfb6834824be93039887110cf821531f7b42277fcb6b75e23`。
另外从 Q8B 实际 OTA ESP 提取启动器重跑同项测试也通过，其 SHA-256 为
`d43b8d259e675935fa280326f8224d0debb44516cf66c61af9ba989d4316ca76`。

## 实际构建与发布产物

在激活 SDK 后，以 `BB_NUMBER_THREADS = "4"`、`PARALLEL_MAKE = "-j4"`
构建；LLVM/Clang 单包使用 `-j16`，GDB 使用 `-j8`，Q8B 内核增量构建使用 `-j16`。
这些设置通过临时 `-R` 配置传入，没有改写用户的 `conf/local.conf`。
Q8B 的机器选择单独通过 `-r` 预加载，保证加载 SC8280XP 的机器配置；矩阵也在
加载机器配置前选择 MACHINE，并检查 SoC、DTB 和 UKI DTB。

| 检查 | 结果 |
| --- | --- |
| Q6A `bitbake radxa-dragon-q6a-flash-bundle` | 8,676 个任务完成；SD、UFS、OTA 文件系统、仓库归档和 bundle 均生成成功 |
| `audit-radxa-dragon-q6a.py tmp/deploy/images/radxa-dragon-q6a` | 完整审计通过，bundle 中 36 项校验和通过；分区与 OTA 原始文件逐字节一致 |
| Q6A SD 实际 commit 经 `qcom-ota-release.py` 发布至临时目录 | 临时测试密钥签名、仓库 fsck、全新消费者拉取及验签通过；未上传服务器 |
| Q8B `bitbake -r <q8b-machine.conf> radxa-dragon-q8b-flash-bundle` | 8,575 个任务完成；专用 DTB、SD/UFS UKI、OTA 文件系统及 bundle 均生成成功 |
| `audit-radxa-dragon-q8b.py tmp/deploy/images/radxa-dragon-q8b` | 完整审计通过，UKI 内嵌 DTB 与 SC8280XP/Q8B 专用 DTB 一致，bundle 中 38 项校验和通过 |
| Q8B SD 实际 commit 经 `qcom-ota-release.py` 发布至临时目录 | 签名、fsck、全新消费者拉取及验签通过；未上传服务器 |

Q6A 两种镜像的 rootfs 实际可用空间均为 1,899,143,168 字节，超过当前完整更新
所需的约 1,570,743,726 字节；该数值已扣除 ext4 保留块。最终 SD commit：
`d40497c23d4c8b740566cc2d40ac23ac88f29719937e317028510a324e457ce0`；
UFS commit：`5318d7276c9450e47a8f9ea5552d88ec6c9dfca13b2f227b865d22339839bd24`。
对应 WIC SHA-256：

- SD：`0fbc93f9260fb7131c76dd98a98a55e6a139d8627b7cbbcd26f88cf4ac126457`
- UFS：`714ef075498e364531914eae01b56959bc50abeb6301a91dc8a9add123ba270b`

Q8B SD/UFS 的 rootfs 实际可用空间分别为 2,046,066,688 和 2,046,058,496 字节，
超过当前完整更新所需的约 1,708,046,972 字节。SD commit：
`cb0d242fbdffae293e0cf4d008e5d46fb2deaa3bb083dcaf9221dd8c15f4c13b`；
UFS commit：`59b414fe34a2133e632930810e15372432974f8eee7f6ef16807c9187d3d66f3`。
对应 WIC SHA-256：

- SD：`47e4eb4cd9d64dc7203d046ab613e11d6b49e27556d53308281cb8d5897199a6`
- UFS：`c8495d64d2e6637f5c4b5343279af6724d7a8e2405098b565e1c4db3edd4a9f9`

## CI 环境限制

首次按层的最小 `ci/base.yml` 运行全部 `ci/oe-selftest.sh`：24 项中 11 项成功、
4 项 OTA 测试明确跳过、9 项失败。失败发生在已有 EFI 镜像配方解析阶段：
缺少 `recipes-products/images/qcom-minimal-image.bb`，最小 BSP CI 没有加载
`meta-qcom-distro` 等完整发行版层。不能将这次全量 CI 报为通过。
定向重跑 `ci/oe-selftest.sh ... qcom_ota`，6 项均因缺少
完整发行版层而明确跳过；不能把退出码成功当作 OTA 测试通过。
SDK 的完整层组合使用上面的 38 项矩阵单独验证 OTA 元数据。

本工作区把该层嵌套在 SDK Git 仓库内，CI helper 的 `/repo/ci/` 路径不成立；
实际使用 kas 的直接 shell 方式调用 `/repo/layers/meta-radxa-dragon/ci/` 下的脚本。
容器使用 Docker，下载和 sstate 缓存在仓库外。

## 实板验收要求

每个实际产品板型仍需验证首次迁移、连续升级、健康确认、三次失败启动、断电恢复、
存储容量和关键外设。ARMv7 仅有固定内核下的用户空间 OTA，不支持启动失败自动回退。
发布工具的验签成功只说明软件发布内容一致，不构成硬件验收证明。
