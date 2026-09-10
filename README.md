# Qualcomm Yocto eSDK for Radxa Dragon Q6A and Q8B

This repository is a Qualcomm Linux 2.0 Yocto extensible SDK (eSDK) for
developing and building images for Radxa Dragon boards. It contains the
OpenEmbedded layers, cross-development sysroots, BitBake configuration, and
local build state needed to work on board support from one checkout.

## Supported Boards

| Machine | SoC | Selection |
| --- | --- | --- |
| `radxa-dragon-q6a` | Qualcomm QCS6490 | Default in `conf/local.conf` |
| `radxa-dragon-q8b` | Qualcomm SC8280XP | Select through `MACHINE` |

Use a Linux host with sufficient storage for Yocto builds; 100 GiB of free
space is recommended. Start from a fresh shell, and unset `LD_LIBRARY_PATH` if
it is defined because the SDK environment rejects it.

## Repository Layout

- `conf/` contains the active distro, machine, and layer configuration.
- `layers/meta-radxa-dragon/` contains board machines, recipes, CI definitions,
  tests, and board-specific documentation.
- `layers/meta-qcom-distro/` supplies the Qualcomm distribution policy.
- `layers/oe-core/` provides BitBake and the OpenEmbedded core metadata.
- `apps/` is the permanent YAML-managed application layer. Each application
  can carry its own userspace sources, systemd units, kernel configuration,
  external modules, and kernel/device-tree patches.
- `workspace/` is managed by `devtool` for checked-out recipe sources.
- `tmp/`, `downloads/`, `sstate-cache/`, and `cache/` are generated build state.

Do not edit generated directories directly. Make persistent changes in the
appropriate layer or through `devtool`.

## Build Q6A Images

Q6A is the default machine. Activate the SDK and build the minimal SD and UFS
images:

```sh
source ./environment-setup-armv8a-qcom-linux
devtool build-image qcom-minimal-efi-sd-image
devtool build-image qcom-minimal-efi-ufs-4k-image
```

## Build Q8B Images

In a fresh shell, activate the SDK and pass the Q8B machine selection into
BitBake:

```sh
source ./environment-setup-armv8a-qcom-linux
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS MACHINE"
export MACHINE=radxa-dragon-q8b
layers/oe-core/bitbake/bin/bitbake qcom-minimal-efi-sd-image
layers/oe-core/bitbake/bin/bitbake qcom-minimal-efi-ufs-4k-image
```

For either board, proprietary multimedia variants are available as
`qcom-multimedia-proprietary-efi-sd-image` and
`qcom-multimedia-proprietary-efi-ufs-4k-image`. Completed artifacts are placed
under `tmp/deploy/images/<machine>/`.

## Recipe Development

Use the eSDK workflow to modify and rebuild individual recipes:

```sh
source ./environment-setup-armv8a-qcom-linux
devtool modify <recipe>
devtool build <recipe>
```

Preserve wanted source changes before running `devtool reset <recipe>`. Read
[Repository Guidelines](AGENTS.md) and the layer-level
[agent guide](layers/meta-radxa-dragon/AGENTS.md) before making broader changes.

## Application Development

Applications are selected one at a time and do not require edits to
`conf/local.conf`. The manifest declares compatible machines, kernel providers,
and image targets, so an invalid board/application combination fails instead
of silently omitting the application:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app list
scripts/qcom-app validate --app example --machine radxa-dragon-q6a
scripts/qcom-app build --app example --machine radxa-dragon-q6a
scripts/qcom-app all --app example --machine radxa-dragon-q6a
```

See [the application layer guide](apps/README.md) for the complete YAML schema,
CMake/Meson support, kernel and device-tree integration, and reference app.

## Validation

The layer provides containerized checks that mirror CI. From
`layers/meta-radxa-dragon/`, with `kas-container` and a working Docker or Podman
runtime, run:

```sh
ci/kas-container-shell-helper.sh ci/yocto-patchreview.sh
ci/kas-container-shell-helper.sh ci/oe-selftest.sh
ci/kas-container-shell-helper.sh ci/yocto-check-layer.sh
```

Run the first two checks routinely and the full layer check before submitting a
pull request. See the [Q6A guide](layers/meta-radxa-dragon/README-Radxa-Dragon-Q6A.md),
[Q8B guide](layers/meta-radxa-dragon/README-Radxa-Dragon-Q8B.md), and
[contribution policy](layers/meta-radxa-dragon/CONTRIBUTING.md) for details.
