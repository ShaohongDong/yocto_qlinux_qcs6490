# SPDX-License-Identifier: MIT
# Source hashes identify inputs; they cannot order package upgrades.
python __anonymous() {
    if d.getVar("QCOM_APP") != "app013-openhd" or d.getVar("MACHINE") != "radxa-dragon-q6a":
        return
    # Epoch 1 migrates from the shared recipe's 0:1.0+app<digest> versions.
    # Bump PV/PR for subsequent released packages; keep source identity below.
    d.setVar("PE", "1")
    d.setVar("PV", "0.1.0")
    d.setVar("PR", "r0")
    d.appendVarFlag("do_install", "postfuncs", " q6a_openhd_install_app_identity")
}

python q6a_openhd_install_app_identity() {
    import json
    from pathlib import Path
    destination = Path(d.getVar("D") + d.getVar("datadir")) / "qcom-selected-app/q6a-openhd"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "build.json").write_text(json.dumps({
        "schema_version": 1,
        "app": d.getVar("QCOM_APP"),
        "app_input_digest": d.getVar("QCOM_APP_INPUT_DIGEST"),
        "package_epoch": d.getVar("PE"),
        "package_version": d.getVar("PV"),
        "package_revision": d.getVar("PR"),
        "wireless_qualified": False,
    }, indent=2) + "\n")
}
