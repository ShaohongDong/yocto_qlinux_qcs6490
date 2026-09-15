# SPDX-License-Identifier: MIT
# Keep the deployed OTA image identity while replacing its desktop session.
IMAGE_FEATURES:remove:radxa-dragon-q6a = "weston"
IMAGE_FEATURES:append:radxa-dragon-q6a = " x11"
CORE_IMAGE_EXTRA_INSTALL:append:radxa-dragon-q6a = " packagegroup-xfce-base qcom-xfce"
SYSTEMD_DEFAULT_TARGET:radxa-dragon-q6a = "graphical.target"

ROOTFS_POSTPROCESS_COMMAND:append:radxa-dragon-q6a = " qcom_xfce_health;"
python qcom_xfce_health() {
    import json
    from pathlib import Path
    path = Path(d.getVar("IMAGE_ROOTFS") + d.getVar("sysconfdir")) / "qcom-ota/health.json"
    health = json.loads(path.read_text())
    health["services"] = list(dict.fromkeys(health.get("services", []) + ["qcom-xfce-ready.service"]))
    path.write_text(json.dumps(health, indent=2) + "\n")
    # Mask after systemd preset-all; earlier masks make rootfs log checks fail.
    units = path.parent.parent / "systemd/system"
    for name in ("weston.service", "weston.socket"):
        mask = units / name
        mask.unlink(missing_ok=True)
        mask.symlink_to("/dev/null")
}
