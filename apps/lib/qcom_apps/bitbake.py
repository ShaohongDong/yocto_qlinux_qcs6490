"""BitBake integration for the selected QCOM application."""

from __future__ import annotations

import shlex
import shutil
from pathlib import Path

from .manifest import ManifestError, load_manifest


def _selected(d, required: bool = False):
    name = (d.getVar("QCOM_APP") or "").strip()
    if not name:
        if required:
            import bb
            bb.fatal("QCOM_APP is empty; select one application with scripts/qcom-app")
        return None
    try:
        spec = load_manifest(d.getVar("QCOM_APPS_DIR"), name)
        import bb

        bb.parse.mark_dependency(d, __file__)
        bb.parse.mark_dependency(d, str(Path(__file__).with_name("manifest.py")))
        for dependency in spec.parse_dependencies():
            bb.parse.mark_dependency(d, str(dependency))
        machine = d.getVar("MACHINE") or ""
        config = spec.machine(machine)
        return spec, config
    except ManifestError as error:
        import bb
        bb.fatal(f"invalid QCOM application selection: {error}")


def build_class(d) -> str:
    selected = _selected(d)
    return selected[0].build["system"] if selected else ""


def _validate_provider(d, spec, config) -> str:
    import bb

    provider = (d.getVar("PREFERRED_PROVIDER_virtual/kernel") or "").strip()
    if not provider:
        bb.fatal(f"machine {d.getVar('MACHINE')} does not select PREFERRED_PROVIDER_virtual/kernel")
    if provider not in config["kernel_providers"]:
        bb.fatal(
            f"application {spec.name} does not support kernel provider {provider} on "
            f"{d.getVar('MACHINE')}; allowed: {', '.join(config['kernel_providers'])}"
        )
    return provider


def _source_uris(spec, paths) -> str:
    uris = ["file://app.yaml;subdir=${BP};apply=no"]
    uris.extend(f"file://{path};subdir=${{BP}};apply=no" for path in paths)
    return " ".join(uris)


def populate_app_recipe(d) -> None:
    selected = _selected(d)
    if not selected:
        d.setVar("COMPATIBLE_MACHINE", "^$")
        return
    spec, config = selected
    _validate_provider(d, spec, config)

    d.setVar("COMPATIBLE_MACHINE", ".*")
    d.setVar("SUMMARY", spec.summary)
    d.setVar("DESCRIPTION", spec.summary)
    d.setVar("LICENSE", spec.license["expression"])
    d.setVar(
        "LIC_FILES_CHKSUM",
        " ".join(f"file://{entry['path']};md5={entry['md5']}" for entry in spec.license["files"]),
    )
    d.prependVar("FILESEXTRAPATHS", f"{spec.root}:")
    d.setVar("SRC_URI", _source_uris(spec, spec.inputs))
    d.setVar("S", "${UNPACKDIR}/${BP}")
    source = "${S}" if spec.build["source"] == "." else f"${{S}}/{spec.build['source']}"
    d.setVar("OECMAKE_SOURCEPATH", source)
    d.setVar("MESON_SOURCEPATH", source)
    options = " ".join(shlex.quote(option) for option in spec.build["options"])
    if spec.build["system"] == "cmake":
        d.appendVar("EXTRA_OECMAKE", " " + options if options else "")
    else:
        d.appendVar("EXTRA_OEMESON", " " + options if options else "")
    d.appendVar("DEPENDS", " " + " ".join(spec.dependencies["build"]))
    d.appendVar(f"RDEPENDS:{d.getVar('PN')}", " " + " ".join(spec.dependencies["runtime"]))
    d.setVar("QCOM_APP_INPUT_DIGEST", spec.digest)
    d.setVar("QCOM_APP_CHECK_EXECUTABLE", spec.check[0] if spec.check else "")
    d.setVar("QCOM_APP_CHECK_ARGUMENTS", " ".join(shlex.quote(arg) for arg in spec.check[1:]))

    enabled = [Path(entry["source"]).name for entry in spec.services if entry["enable"]]
    d.setVar(f"SYSTEMD_SERVICE:{d.getVar('PN')}", " ".join(enabled))
    d.setVar("SYSTEMD_PACKAGES", d.getVar("PN") if enabled else "")
    d.setVar(f"SYSTEMD_AUTO_ENABLE:{d.getVar('PN')}", "enable")
    if spec.services:
        d.appendVar(f"FILES:{d.getVar('PN')}", " ${systemd_system_unitdir}/*")


def install_services(d) -> None:
    selected = _selected(d)
    if not selected:
        return
    spec, _ = selected
    if not spec.services:
        return
    destination = Path(d.getVar("D") + d.getVar("systemd_system_unitdir"))
    destination.mkdir(parents=True, exist_ok=True)
    for entry in spec.services:
        target = destination / Path(entry["source"]).name
        shutil.copy2(spec.root / entry["source"], target)
        target.chmod(0o644)


def validate_install(d) -> None:
    import bb

    selected = _selected(d)
    if not selected:
        return
    spec, _ = selected
    destination = Path(d.getVar("D"))
    if spec.check:
        installed = destination / spec.check[0].lstrip("/")
        if not installed.is_file():
            bb.fatal(f"application check executable was not installed: {spec.check[0]}")
    for entry in spec.services:
        installed = destination / d.getVar("systemd_system_unitdir").lstrip("/") / Path(entry["source"]).name
        if not installed.is_file():
            bb.fatal(f"systemd unit was not installed: {entry['source']}")


def populate_module_recipe(d) -> None:
    selected = _selected(d)
    if not selected:
        d.setVar("COMPATIBLE_MACHINE", "^$")
        return
    spec, config = selected
    _validate_provider(d, spec, config)
    modules = spec.kernel["modules"]
    if not modules:
        d.setVar("COMPATIBLE_MACHINE", "^$")
        return
    d.setVar("COMPATIBLE_MACHINE", ".*")
    d.setVar("SUMMARY", f"External kernel modules for {spec.name}")
    d.setVar("LICENSE", spec.license["expression"])
    d.setVar(
        "LIC_FILES_CHKSUM",
        " ".join(f"file://{entry['path']};md5={entry['md5']}" for entry in spec.license["files"]),
    )
    module_inputs = ["app.yaml"] + [entry["path"] for entry in spec.license["files"]] + modules
    d.prependVar("FILESEXTRAPATHS", f"{spec.root}:")
    d.setVar("SRC_URI", _source_uris(spec, [path for path in module_inputs if path != "app.yaml"]))
    d.setVar("S", "${UNPACKDIR}/${BP}")
    d.setVar("QCOM_APP_MODULE_DIRS", " ".join(modules))
    d.setVar("QCOM_APP_INPUT_DIGEST", spec.digest)


def populate_kernel_recipe(d) -> None:
    selected = _selected(d)
    if not selected:
        return
    spec, config = selected
    provider = _validate_provider(d, spec, config)
    if d.getVar("PN") != provider:
        return

    d.setVar("QCOM_APP_KERNEL_ACTIVE", "1")
    d.setVar("QCOM_APP_INPUT_DIGEST", spec.digest)
    d.prependVar("FILESEXTRAPATHS", f"{spec.root}:")
    uris = []
    for index, path in enumerate(spec.kernel["config"]):
        uris.append(f"file://{path};subdir=qcom-app-config/{index};apply=no")
    for index, entry in enumerate(spec.kernel["device_tree"]["files"]):
        uris.append(f"file://{entry['source']};subdir=qcom-app-dts/{index};apply=no")
    patches = spec.kernel["patches"] + spec.kernel["device_tree"]["patches"]
    for index, path in enumerate(patches):
        uris.append(f"file://{path};striplevel=1;pname=qcom-app-{spec.name}-{index}")
    if uris:
        d.appendVar("SRC_URI", " " + " ".join(uris))
    fragments = [f"${{UNPACKDIR}}/qcom-app-config/{index}/{path}"
                 for index, path in enumerate(spec.kernel["config"])]
    d.setVar("QCOM_APP_CONFIG_FRAGMENTS", " ".join(fragments))
    if spec.kernel["config"]:
        postfuncs = d.getVarFlag("do_configure", "postfuncs") or ""
        d.setVarFlag(
            "do_configure", "postfuncs",
            postfuncs + " qcom_app_merge_kernel_config qcom_app_verify_kernel_config",
        )


def stage_device_tree(d) -> None:
    selected = _selected(d)
    if not selected or d.getVar("QCOM_APP_KERNEL_ACTIVE") != "1":
        return
    spec, _ = selected
    unpackdir = Path(d.getVar("UNPACKDIR"))
    source_tree = Path(d.getVar("S"))
    for index, entry in enumerate(spec.kernel["device_tree"]["files"]):
        source = unpackdir / "qcom-app-dts" / str(index) / entry["source"]
        target = source_tree / entry["target"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def image_selection(d) -> None:
    selected = _selected(d)
    if not selected:
        return
    spec, _ = selected
    try:
        target = spec.image(d.getVar("MACHINE"), (d.getVar("QCOM_APP_IMAGE") or "").strip() or None)
    except ManifestError as error:
        import bb
        bb.fatal(str(error))
    d.setVar("QCOM_APP_RESOLVED_IMAGE", target)
    if d.getVar("PN") != target:
        return
    d.appendVar("IMAGE_INSTALL", " packagegroup-qcom-selected-app")
    d.setVar("QCOM_APP_IMAGE_ACTIVE", "1")
    d.setVar("QCOM_APP_CHECK_EXECUTABLE", spec.check[0] if spec.check else "")
    d.setVar("QCOM_APP_CHECK_ARGUMENTS", " ".join(shlex.quote(arg) for arg in spec.check[1:]))
    if spec.check:
        d.appendVarFlag("do_rootfs", "depends", " qemu-native:do_populate_sysroot")
        d.appendVar("ROOTFS_POSTPROCESS_COMMAND", " qcom_app_run_rootfs_check;")


def packagegroup_dependencies(d) -> str:
    selected = _selected(d)
    if not selected:
        return ""
    spec, config = selected
    _validate_provider(d, spec, config)
    dependencies = ["qcom-selected-app"]
    if spec.kernel["modules"]:
        dependencies.append("qcom-selected-app-modules")
    return " ".join(dependencies)
