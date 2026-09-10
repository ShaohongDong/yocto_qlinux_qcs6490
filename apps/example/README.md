# QCOM application framework example

This application exercises the complete manifest contract: a CMake userspace
program, an external kernel module, an enabled systemd service, a Kconfig
fragment, one documentation-only kernel patch, and one disabled device-tree
node. The device-tree example is intentionally inert and does not claim board
functionality.

From a fresh SDK shell, validate and build it with:

```sh
source ./environment-setup-armv8a-qcom-linux
scripts/qcom-app validate --app example --machine radxa-dragon-q6a
scripts/qcom-app all --app example --machine radxa-dragon-q6a
```
