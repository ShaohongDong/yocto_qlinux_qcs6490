# QCOM application layer

`apps/` is both the permanent application workspace and an OpenEmbedded layer.
Each `apps/<name>/app.yaml` is the source of truth for one application. The
shared recipes and classes turn the selected manifest into userspace packages,
external kernel modules, kernel and device-tree changes, systemd units, and an
image dependency.

Only one application is selected per invocation. An unselected application
does not modify `SRC_URI`, the kernel configuration, or an image.

## Commands

Activate the eSDK in a fresh shell, then use the wrapper:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app list
scripts/qcom-app validate --app example --machine radxa-dragon-q6a
scripts/qcom-app build --app example --machine radxa-dragon-q6a
scripts/qcom-app kernel --app example --machine radxa-dragon-q6a
scripts/qcom-app image --app example --machine radxa-dragon-q6a
scripts/qcom-app all --app example --machine radxa-dragon-q6a
```

`image` and `all` use the machine's default image from the manifest. Pass
`--image <target>` to choose another target from that machine's `allowed`
list. The wrapper passes selection through a temporary BitBake post-config and
does not rewrite `conf/local.conf`.

## Manifest contract

The schema is strict and currently fixed at version 1:

```yaml
schema_version: 1
name: my-app
summary: Short package description
license:
  expression: MIT
  files:
  - path: LICENSE
    md5: 0123456789abcdef0123456789abcdef
inputs: [LICENSE, CMakeLists.txt, src, kernel, systemd]
build:
  system: cmake                # cmake or meson
  source: .
  options: []                  # structured build-system arguments, not shell
check: [/usr/bin/my-app, --self-test]
dependencies:
  build: [libfoo]
  runtime: [libfoo]
machines:
  radxa-dragon-q6a:
    kernel_providers: [linux-qcom]
    images:
      default: qcom-minimal-efi-sd-image
      allowed: [qcom-minimal-efi-sd-image]
kernel:
  config: [kernel/config/my-app.cfg]
  patches: [kernel/patches/0001-add-driver.patch]
  device_tree:
    files:
    - source: kernel/dts/my-app.dtsi
      target: arch/arm64/boot/dts/qcom/my-app.dtsi
    patches: [kernel/patches/0002-include-my-app-dtsi.patch]
  modules: [kernel/modules/my-app]
services:
- source: systemd/my-app.service
  enable: true
  wanted_by: multi-user.target
```

`inputs` is the portable source allowlist and includes directories
recursively; `app.yaml` is implicit. Hidden inputs, symlinks, path traversal,
duplicate YAML keys, unknown fields, arbitrary YAML objects, unsafe build
options, and references outside the allowlist are rejected. License MD5 values
are checked against the local files.

Kernel and device-tree patches use normal BitBake `SRC_URI` processing and are
therefore part of task signatures. Device-tree files are copied into the
declared kernel-relative targets before patches are applied. Kconfig fragments
must use the `.cfg` suffix and their final values are verified after
`olddefconfig`.

Every machine entry is an explicit compatibility contract. Selection fails if
the active machine's `PREFERRED_PROVIDER_virtual/kernel` is not allowed or if
the requested image is outside the allowlist. Runtime checks must be suitable
for QEMU user-mode execution and must not require physical peripherals.

## Adding an application

Copy `apps/example/` to a lowercase kebab-case directory, change `name`, and
replace its sources and metadata. Keep application-specific kernel and DT
changes inside that directory. Do not add persistent application work to the
devtool-managed `workspace/` layer.

Offline parsing and QEMU checks do not establish physical-board behaviour.
Attach boot, driver, and peripheral logs separately when making hardware
claims.
