"""Strict parser for ``apps/<name>/app.yaml`` manifests."""

from __future__ import annotations

import hashlib
import os
import re
from collections.abc import Iterable
from pathlib import Path, PurePosixPath
from typing import Any

import yaml

NAME_RE = re.compile(r"^[a-z][a-z0-9-]*$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._-]*$")
PATH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._/-]*$")
MD5_RE = re.compile(r"^[0-9a-f]{32}$")
UNSAFE_OPTION_RE = re.compile(r"[\n\r;&|`$<>]")


class ManifestError(RuntimeError):
    """Raised when an application manifest violates the schema."""


class _UniqueKeyLoader(yaml.SafeLoader):
    pass


def _construct_mapping(loader: yaml.SafeLoader, node: yaml.MappingNode, deep: bool = False) -> dict:
    mapping: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise ManifestError(f"duplicate YAML key: {key}")
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


_UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping
)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ManifestError(message)


def _mapping(value: Any, field: str, allowed: Iterable[str], required: Iterable[str] = ()) -> dict:
    _require(isinstance(value, dict), f"{field} must be a mapping")
    allowed_set = set(allowed)
    required_set = set(required)
    unknown = set(value) - allowed_set
    missing = required_set - set(value)
    _require(not unknown, f"{field} has unknown keys: {', '.join(sorted(unknown))}")
    _require(not missing, f"{field} is missing keys: {', '.join(sorted(missing))}")
    return value


def _string(value: Any, field: str, *, allow_empty: bool = False) -> str:
    _require(isinstance(value, str), f"{field} must be a string")
    _require(allow_empty or bool(value.strip()), f"{field} must not be empty")
    return value


def _strings(value: Any, field: str, *, tokens: bool = False) -> list[str]:
    _require(isinstance(value, list), f"{field} must be a list")
    result = [_string(item, f"{field}[{index}]") for index, item in enumerate(value)]
    _require(len(result) == len(set(result)), f"{field} contains duplicate values")
    if tokens:
        for item in result:
            _require(bool(TOKEN_RE.fullmatch(item)), f"{field} contains invalid token: {item}")
    return result


def _relative_path(value: Any, field: str, *, allow_dot: bool = False) -> str:
    text = _string(value, field)
    path = PurePosixPath(text)
    _require(not path.is_absolute(), f"{field} must be relative")
    _require(text == "." and allow_dot or text != ".", f"{field} must not be '.'")
    _require(text == "." or bool(PATH_RE.fullmatch(text)),
             f"{field} contains unsafe path characters")
    _require(text == "." or path.as_posix() == text,
             f"{field} must use a normalized POSIX path")
    _require(".." not in path.parts, f"{field} must not contain '..'")
    _require(not any(part.startswith(".") for part in path.parts if part != "."),
             f"{field} must not contain hidden path components")
    _require(not re.search(r"[\s;|:]", text),
             f"{field} must not contain whitespace, ';', '|' or ':'")
    return text


def _is_covered(path: str, inputs: list[str]) -> bool:
    candidate = PurePosixPath(path)
    for item in inputs:
        root = PurePosixPath(item)
        if candidate == root or root in candidate.parents:
            return True
    return False


class AppManifest:
    """Validated application manifest and resolved input metadata."""

    def __init__(self, manifest_path: Path):
        manifest_path = Path(manifest_path)
        _require(not manifest_path.is_symlink(), "app.yaml must not be a symbolic link")
        _require(not manifest_path.parent.is_symlink(), "application directory must not be a symbolic link")
        self.path = manifest_path.resolve()
        self.root = self.path.parent
        try:
            raw = yaml.load(self.path.read_text(encoding="utf-8"), Loader=_UniqueKeyLoader)
        except ManifestError:
            raise
        except (OSError, yaml.YAMLError) as error:
            raise ManifestError(f"cannot read {manifest_path}: {error}") from error

        self.data = _mapping(
            raw,
            "app.yaml",
            {
                "schema_version", "name", "summary", "license", "inputs", "build",
                "check", "dependencies", "machines", "kernel", "services",
            },
            {"schema_version", "name", "summary", "license", "inputs", "build", "machines"},
        )
        _require(type(self.data["schema_version"]) is int and self.data["schema_version"] == 1,
                 "schema_version must be integer 1")
        self.name = _string(self.data["name"], "name")
        _require(bool(NAME_RE.fullmatch(self.name)), "name must match [a-z][a-z0-9-]*")
        _require(self.root.name == self.name, "name must match the application directory")
        self.summary = _string(self.data["summary"], "summary")

        self.inputs = [_relative_path(item, f"inputs[{index}]")
                       for index, item in enumerate(_strings(self.data["inputs"], "inputs"))]
        _require(len(self.inputs) == len(set(self.inputs)), "inputs contains duplicate paths")

        self.license = self._parse_license(self.data["license"])
        self.build = self._parse_build(self.data["build"])
        self.check = self._parse_check(self.data.get("check", []))
        self.dependencies = self._parse_dependencies(self.data.get("dependencies", {}))
        self.machines = self._parse_machines(self.data["machines"])
        self.kernel = self._parse_kernel(self.data.get("kernel", {}))
        self.services = self._parse_services(self.data.get("services", []))

        self._validate_inputs()
        self._validate_references()
        self.digest = self._digest()

    def _parse_license(self, value: Any) -> dict:
        value = _mapping(value, "license", {"expression", "files"}, {"expression", "files"})
        expression = _string(value["expression"], "license.expression")
        _require("\n" not in expression and "\r" not in expression,
                 "license.expression must be one line")
        files = value["files"]
        _require(isinstance(files, list) and files, "license.files must be a non-empty list")
        parsed = []
        for index, entry in enumerate(files):
            entry = _mapping(entry, f"license.files[{index}]", {"path", "md5"}, {"path", "md5"})
            path = _relative_path(entry["path"], f"license.files[{index}].path")
            md5 = _string(entry["md5"], f"license.files[{index}].md5")
            _require(bool(MD5_RE.fullmatch(md5)), f"license.files[{index}].md5 must be lowercase MD5")
            parsed.append({"path": path, "md5": md5})
        _require(len(parsed) == len({entry["path"] for entry in parsed}),
                 "license.files contains duplicate paths")
        return {"expression": expression, "files": parsed}

    def _parse_build(self, value: Any) -> dict:
        value = _mapping(value, "build", {"system", "source", "options"}, {"system", "source"})
        system = _string(value["system"], "build.system")
        _require(system in {"cmake", "meson"}, "build.system must be cmake or meson")
        source = _relative_path(value["source"], "build.source", allow_dot=True)
        options = _strings(value.get("options", []), "build.options")
        for option in options:
            _require(not UNSAFE_OPTION_RE.search(option), f"unsafe build option: {option}")
        return {"system": system, "source": source, "options": options}

    def _parse_check(self, value: Any) -> list[str]:
        check = _strings(value, "check")
        if check:
            executable = PurePosixPath(check[0])
            _require(executable.is_absolute() and str(executable).startswith("/usr/"),
                     "check executable must be an absolute path below /usr")
            _require(bool(PATH_RE.fullmatch(str(executable).lstrip("/"))) and
                     str(executable) == executable.as_posix(),
                     "check executable contains unsafe or non-normalized path characters")
            _require(".." not in executable.parts, "check executable must not contain '..'")
        return check

    def _parse_dependencies(self, value: Any) -> dict:
        value = _mapping(value, "dependencies", {"build", "runtime"})
        return {
            "build": _strings(value.get("build", []), "dependencies.build", tokens=True),
            "runtime": _strings(value.get("runtime", []), "dependencies.runtime", tokens=True),
        }

    def _parse_machines(self, value: Any) -> dict[str, dict]:
        _require(isinstance(value, dict) and value, "machines must be a non-empty mapping")
        result = {}
        for machine, config in value.items():
            _require(isinstance(machine, str) and TOKEN_RE.fullmatch(machine) is not None,
                     f"invalid machine name: {machine}")
            config = _mapping(
                config, f"machines.{machine}", {"kernel_providers", "images"},
                {"kernel_providers", "images"},
            )
            providers = _strings(config["kernel_providers"],
                                 f"machines.{machine}.kernel_providers", tokens=True)
            _require(bool(providers), f"machines.{machine}.kernel_providers must not be empty")
            images = _mapping(config["images"], f"machines.{machine}.images",
                              {"default", "allowed"}, {"default", "allowed"})
            default = _string(images["default"], f"machines.{machine}.images.default")
            allowed = _strings(images["allowed"], f"machines.{machine}.images.allowed", tokens=True)
            _require(default in allowed, f"default image for {machine} must be in allowed")
            result[machine] = {
                "kernel_providers": providers,
                "images": {"default": default, "allowed": allowed},
            }
        return result

    def _parse_kernel(self, value: Any) -> dict:
        value = _mapping(value, "kernel", {"config", "patches", "device_tree", "modules"})
        config = [_relative_path(item, f"kernel.config[{index}]")
                  for index, item in enumerate(_strings(value.get("config", []), "kernel.config"))]
        patches = [_relative_path(item, f"kernel.patches[{index}]")
                   for index, item in enumerate(_strings(value.get("patches", []), "kernel.patches"))]
        for path in config:
            _require(path.endswith(".cfg"), f"kernel config fragment must end in .cfg: {path}")
        for path in patches:
            _require(path.endswith((".patch", ".diff")), f"kernel patch has invalid suffix: {path}")

        device_tree = _mapping(value.get("device_tree", {}), "kernel.device_tree", {"files", "patches"})
        files_value = device_tree.get("files", [])
        _require(isinstance(files_value, list), "kernel.device_tree.files must be a list")
        dt_files = []
        for index, entry in enumerate(files_value):
            entry = _mapping(entry, f"kernel.device_tree.files[{index}]",
                             {"source", "target"}, {"source", "target"})
            source = _relative_path(entry["source"], f"kernel.device_tree.files[{index}].source")
            target = _relative_path(entry["target"], f"kernel.device_tree.files[{index}].target")
            _require(source.endswith((".dts", ".dtsi")), f"device-tree source has invalid suffix: {source}")
            _require(re.match(r"^arch/[A-Za-z0-9_-]+/boot/dts/", target) is not None,
                     f"device-tree target must be below arch/<arch>/boot/dts: {target}")
            _require(target.endswith((".dts", ".dtsi")), f"device-tree target has invalid suffix: {target}")
            dt_files.append({"source": source, "target": target})
        dt_patches = [_relative_path(item, f"kernel.device_tree.patches[{index}]")
                      for index, item in enumerate(_strings(device_tree.get("patches", []),
                                                            "kernel.device_tree.patches"))]
        for path in dt_patches:
            _require(path.endswith((".patch", ".diff")), f"device-tree patch has invalid suffix: {path}")

        modules = [_relative_path(item, f"kernel.modules[{index}]")
                   for index, item in enumerate(_strings(value.get("modules", []), "kernel.modules"))]
        all_paths = config + patches + dt_patches + modules + [entry["source"] for entry in dt_files]
        _require(len(all_paths) == len(set(all_paths)), "kernel inputs contain duplicate paths")
        targets = [entry["target"] for entry in dt_files]
        _require(len(targets) == len(set(targets)), "kernel.device_tree.files contains duplicate targets")
        return {
            "config": config,
            "patches": patches,
            "device_tree": {"files": dt_files, "patches": dt_patches},
            "modules": modules,
        }

    def _parse_services(self, value: Any) -> list[dict]:
        _require(isinstance(value, list), "services must be a list")
        result = []
        sources = []
        for index, entry in enumerate(value):
            entry = _mapping(entry, f"services[{index}]", {"source", "enable", "wanted_by"},
                             {"source", "enable", "wanted_by"})
            source = _relative_path(entry["source"], f"services[{index}].source")
            _require(source.endswith((".service", ".socket", ".timer", ".path")),
                     f"unsupported systemd unit suffix: {source}")
            _require(type(entry["enable"]) is bool, f"services[{index}].enable must be boolean")
            wanted_by = _string(entry["wanted_by"], f"services[{index}].wanted_by")
            _require(bool(TOKEN_RE.fullmatch(wanted_by)), f"invalid wanted_by target: {wanted_by}")
            result.append({"source": source, "enable": entry["enable"], "wanted_by": wanted_by})
            sources.append(source)
        _require(len(sources) == len(set(sources)), "services contains duplicate source units")
        unit_names = [PurePosixPath(source).name for source in sources]
        _require(len(unit_names) == len(set(unit_names)),
                 "services contains duplicate installed unit names")
        return result

    def _checked_path(self, relative: str, kind: str | None = None) -> Path:
        path = self.root / relative
        try:
            resolved = path.resolve(strict=True)
        except OSError as error:
            raise ManifestError(f"missing input {relative}: {error}") from error
        _require(resolved == self.root or self.root in resolved.parents,
                 f"input escapes application directory: {relative}")
        current = self.root
        for part in PurePosixPath(relative).parts:
            if part == ".":
                continue
            current /= part
            _require(not current.is_symlink(), f"symbolic links are not allowed: {relative}")
        if kind == "file":
            _require(path.is_file(), f"expected file: {relative}")
        elif kind == "dir":
            _require(path.is_dir(), f"expected directory: {relative}")
        return path

    def _validate_inputs(self) -> None:
        for relative in self.inputs:
            path = self._checked_path(relative)
            if path.is_dir():
                for directory, directories, files in os.walk(path, followlinks=False):
                    for name in directories + files:
                        child = Path(directory) / name
                        child_relative = child.relative_to(self.root)
                        _require(not name.startswith("."), f"hidden input is not allowed: {child_relative}")
                        _require(not child.is_symlink(), f"symbolic links are not allowed: {child_relative}")
            else:
                _require(path.is_file(), f"input must be a file or directory: {relative}")

    def _validate_references(self) -> None:
        references = [entry["path"] for entry in self.license["files"]]
        references += [entry["source"] for entry in self.services]
        references += self.kernel["config"] + self.kernel["patches"] + self.kernel["modules"]
        references += self.kernel["device_tree"]["patches"]
        references += [entry["source"] for entry in self.kernel["device_tree"]["files"]]
        for relative in references:
            self._checked_path(relative)
            _require(_is_covered(relative, self.inputs), f"referenced path is not covered by inputs: {relative}")

        for entry in self.license["files"]:
            actual = hashlib.md5(self._checked_path(entry["path"], "file").read_bytes()).hexdigest()
            _require(actual == entry["md5"],
                     f"license checksum mismatch for {entry['path']}: expected {entry['md5']}, got {actual}")

        source = self.root if self.build["source"] == "." else self._checked_path(self.build["source"], "dir")
        marker = source / ("CMakeLists.txt" if self.build["system"] == "cmake" else "meson.build")
        _require(marker.is_file(), f"missing {marker.name} in build.source")
        marker_relative = marker.relative_to(self.root).as_posix()
        _require(_is_covered(marker_relative, self.inputs), f"{marker_relative} is not covered by inputs")

        for module in self.kernel["modules"]:
            module_path = self._checked_path(module, "dir")
            _require((module_path / "Makefile").is_file() or (module_path / "Kbuild").is_file(),
                     f"kernel module directory lacks Makefile or Kbuild: {module}")

        for service in self.services:
            unit = self._checked_path(service["source"], "file").read_text(encoding="utf-8")
            wanted = []
            section = None
            for line in unit.splitlines():
                stripped = line.strip()
                if stripped.startswith("[") and stripped.endswith("]"):
                    section = stripped
                elif section == "[Install]" and stripped.startswith("WantedBy="):
                    wanted.extend(stripped.split("=", 1)[1].split())
            _require(service["wanted_by"] in wanted,
                     f"{service['source']} WantedBy does not contain {service['wanted_by']}")

    def _digest(self) -> str:
        digest = hashlib.sha256()
        digest.update(self.path.read_bytes())
        for relative in sorted(self.expanded_inputs()):
            digest.update(relative.encode("utf-8") + b"\0")
            digest.update((self.root / relative).read_bytes())
        return digest.hexdigest()

    def expanded_inputs(self) -> list[str]:
        """Return all regular input files, relative to the application root."""
        result = {"app.yaml"}
        for relative in self.inputs:
            path = self.root / relative
            if path.is_file():
                result.add(relative)
            else:
                for child in path.rglob("*"):
                    relative_child = child.relative_to(self.root)
                    if child.is_file() and "__pycache__" not in relative_child.parts and child.suffix not in (".pyc", ".pyo"):
                        result.add(relative_child.as_posix())
        return sorted(result)

    def parse_dependencies(self) -> list[Path]:
        """Return files and directories that invalidate BitBake parse caches."""
        result = {self.path}
        for relative in self.inputs:
            path = self.root / relative
            result.add(path)
            if path.is_dir():
                for directory, directories, files in os.walk(path, followlinks=False):
                    root = Path(directory)
                    directories[:] = [name for name in directories if name != "__pycache__"]
                    files = [name for name in files if Path(name).suffix not in (".pyc", ".pyo")]
                    result.add(root)
                    result.update(root / name for name in directories + files)
        return sorted(result)

    def machine(self, name: str) -> dict:
        if name not in self.machines:
            raise ManifestError(
                f"application {self.name} does not support machine {name}; "
                f"supported: {', '.join(sorted(self.machines))}"
            )
        return self.machines[name]

    def image(self, machine: str, override: str | None = None) -> str:
        images = self.machine(machine)["images"]
        target = override or images["default"]
        if target not in images["allowed"]:
            raise ManifestError(
                f"image {target} is not allowed for {self.name} on {machine}; "
                f"allowed: {', '.join(images['allowed'])}"
            )
        return target


def load_manifest(apps_dir: str | Path, name: str) -> AppManifest:
    _require(bool(NAME_RE.fullmatch(name or "")), "QCOM_APP must match [a-z][a-z0-9-]*")
    manifest = Path(apps_dir).resolve() / name / "app.yaml"
    _require(manifest.is_file(), f"application manifest not found: {manifest}")
    return AppManifest(manifest)


def discover_apps(apps_dir: str | Path) -> list[AppManifest]:
    root = Path(apps_dir).resolve()
    manifests = []
    for path in sorted(root.glob("*/app.yaml")):
        if path.parent.name.startswith("."):
            continue
        manifests.append(AppManifest(path))
    return manifests
