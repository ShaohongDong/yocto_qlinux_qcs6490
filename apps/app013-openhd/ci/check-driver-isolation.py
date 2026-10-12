#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check actual BitBake task hashes with/without private app/driver appends."""
import json
import sys
from pathlib import Path

sys.path.insert(0, "/repo/layers/oe-core/bitbake/lib")
import bb.data
import bb.tinfoil


def main():
    with bb.tinfoil.Tinfoil() as session:
        session.prepare(quiet=3)
        recipe = session.get_recipe_file("aic8800-usb")
        appends = session.get_file_appends(recipe)
        ordinary = [path for path in appends if "/app013-openhd/meta-openhd/" not in path]
        assert len(appends) == len(ordinary) + 1, appends
        cases = []
        for selection in ("", "app003-hevc-benchmark", "app012-music-player"):
            config = bb.data.createCopy(session.config_data)
            # Intentionally parse multiple configurations of one filename.
            # Compare hashes ourselves instead of emitting rebuild errors for
            # expected differences between distinct app selections.
            config.setVar("BB_HASH_IGNORE_MISMATCH", "1")
            config.setVar("QCOM_APP", selection)
            baseline = session.parse_recipe_file(recipe, appendlist=ordinary, config_data=config)
            private = session.parse_recipe_file(recipe, appendlist=appends, config_data=config)
            basehash = baseline.getVar("__siggen_basehashes")
            hashes = private.getVar("__siggen_basehashes")
            changes = {task: {"baseline": basehash.get(task), "private": hashes.get(task)}
                       for task in set(basehash) | set(hashes)
                       if basehash.get(task) != hashes.get(task)}
            cases.append({"app": selection, "changed_tasks": changes})
            assert not changes, cases[-1]
        config = bb.data.createCopy(session.config_data)
        config.setVar("BB_HASH_IGNORE_MISMATCH", "1")
        config.setVar("QCOM_APP", "app013-openhd")
        selected = session.parse_recipe_file(recipe, appendlist=appends, config_data=config)
        assert "CONFIG_RWNX_MON_XMIT=y" in selected.getVar("EXTRA_OEMAKE")
        assert "Expose-experimental-monitor" in selected.getVar("SRC_URI")
        assert "q6a_openhd_install_marker" in selected.getVarFlag("do_install", "postfuncs")
        app_recipe = session.get_recipe_file("qcom-selected-app")
        app_appends = session.get_file_appends(app_recipe)
        app_ordinary = [path for path in app_appends if "/app013-openhd/meta-openhd/" not in path]
        assert len(app_appends) == len(app_ordinary) + 1, app_appends
        app_cases = []
        for selection in ("app003-hevc-benchmark", "app012-music-player"):
            config = bb.data.createCopy(session.config_data)
            config.setVar("BB_HASH_IGNORE_MISMATCH", "1")
            config.setVar("QCOM_APP", selection)
            baseline = session.parse_recipe_file(app_recipe, appendlist=app_ordinary, config_data=config)
            private = session.parse_recipe_file(app_recipe, appendlist=app_appends, config_data=config)
            basehash = baseline.getVar("__siggen_basehashes")
            hashes = private.getVar("__siggen_basehashes")
            changes = {task: {"baseline": basehash.get(task), "private": hashes.get(task)}
                       for task in set(basehash) | set(hashes)
                       if basehash.get(task) != hashes.get(task)}
            assert all(baseline.getVar(name) == private.getVar(name) for name in ("PE", "PV", "PR"))
            app_cases.append({"app": selection, "changed_tasks": changes})
            assert not changes, app_cases[-1]
        config = bb.data.createCopy(session.config_data)
        config.setVar("BB_HASH_IGNORE_MISMATCH", "1")
        config.setVar("QCOM_APP", "app013-openhd")
        selected_app = session.parse_recipe_file(app_recipe, appendlist=app_appends, config_data=config)
        assert selected_app.getVar("PE") == "1" and selected_app.getVar("PV") == "0.1.0"
        assert selected_app.getVar("PR") == "r0"
        assert "q6a_openhd_install_app_identity" in selected_app.getVarFlag("do_install", "postfuncs")
        print(json.dumps({"result": "PASS", "other_apps": cases,
                          "app013_monitor_enabled": True, "app_recipe_other_apps": app_cases,
                          "app013_package_version": "1:0.1.0-r0",
                          "app013_package_workdir": selected_app.getVar("WORKDIR")}, indent=2))


if __name__ == "__main__":
    main()
