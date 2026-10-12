# SPDX-License-Identifier: MIT
# Add no task, patch, build flag or package content for other selections.
python __anonymous() {
    if d.getVar("QCOM_APP") != "app013-openhd" or d.getVar("MACHINE") != "radxa-dragon-q6a":
        return
    root = d.getVar("Q6A_OPENHD_LAYERDIR")
    d.prependVar("FILESEXTRAPATHS", root + "/recipes-connectivity/aic8800/files:")
    d.appendVar("SRC_URI", " file://0001-Expose-experimental-monitor-TX-build-status.patch file://0002-Validate-monitor-frame-bounds-before-parsing.patch file://0003-Handle-monitor-forward-flags-without-MON_DATA.patch file://0004-Select-the-started-netdev-queue-for-monitor-TX.patch")
    # oe_runmake uses EXTRA_OEMAKE without changing the common recipe.
    d.appendVar("EXTRA_OEMAKE", " CONFIG_RWNX_MON_XMIT=y")
    d.setVar("PR", d.getVar("PR") + ".openhd3")
    d.appendVarFlag("do_install", "postfuncs", " q6a_openhd_install_marker")
    d.appendVar("FILES:" + d.getVar("PN"), " " + d.getVar("datadir") + "/q6a-openhd/aic8800-build.json")
}

python q6a_openhd_install_marker() {
    import json
    from pathlib import Path
    destination = Path(d.getVar("D") + d.getVar("datadir")) / "q6a-openhd"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "aic8800-build.json").write_text(json.dumps({
        "schema_version": 1,
        "driver_release": d.getVar("AIC8800_DEB_VERSION"),
        "monitor_xmit": True,
        "wireless_qualified": False,
    }) + "\n")
}
