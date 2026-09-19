# SPDX-License-Identifier: MIT
# One owner: the active desktop user, authorized by logind/uaccess.
PACKAGECONFIG:remove:radxa-dragon-q6a = "systemd-system-service"
PACKAGE_ARCH:radxa-dragon-q6a = "${MACHINE_ARCH}"
