#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect read-only Q6A audio evidence, or audit a captured JSON report.

Run on the board as root: audit-q6a-audio.py --pulse-user weston > audio.json
Run offline: audit-q6a-audio.py --check audio.json
An audit pass checks prerequisites, not audible playback acceptance.
"""
import argparse
import hashlib
import fcntl
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET


def command(args):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=10, check=False)
        return {"returncode": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"returncode": -1, "stdout": "", "stderr": str(error)}


def card_records(text):
    cards = []
    for match in re.finditer(r"^\s*(\d+) \[([^\]]+)\]: ([^\n]+)\n[ \t]+([^\n]+)", text, re.M):
        index, card_id, driver_name, long_name = match.groups()
        name = driver_name.split(" - ", 1)[-1]
        cards.append({"index": int(index), "id": card_id.strip(), "name": name, "long_name": long_name.strip()})
    return cards


def suffixes(name):
    parts = []
    previous = ""
    for part in name.split("-"):
        if part.startswith("snd"):
            break
        previous = "_".join(parts)
        parts.append(part)
    return list(dict.fromkeys(filter(None, ["_".join(parts), previous])))


def digest(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:
        return None


def collect(user):
    cards = card_records(Path("/proc/asound/cards").read_text())
    board = next((c for c in cards if c["name"] == "QCS6490-Radxa-Dragon-Q6A"), None)
    data = {"schema_version": 1, "uname": command(["uname", "-a"]), "cards": cards,
            "pcm": command(["cat", "/proc/asound/pcm"]), "board": board}
    try:
        tree = ET.parse("/etc/card-defs.xml")
        data["virtual_cards"] = [node.findtext("name") for node in tree.findall("card")]
    except (OSError, ET.ParseError) as error:
        data["virtual_cards"] = []; data["card_defs_error"] = str(error)
    data["acdb"] = {}; data["pal_configs"] = {}
    if board:
        for suffix in suffixes(board["name"]):
            directory = Path("/etc/acdbdata") / suffix
            data["acdb"][str(directory)] = {str(p): digest(p) for p in directory.glob("*.acdb")}
        for suffix in suffixes(board["name"]):
            for kind in ("mixer_paths", "resourcemanager"):
                path = f"/etc/{kind}_{suffix}.xml"
                data["pal_configs"][path] = digest(path)
        data["controls"] = command(["amixer", "-c", str(board["index"]), "contents"])
    data["jack_switches"] = []
    for name_file in Path("/sys/class/input").glob("event*/device/name"):
        name = name_file.read_text().strip()
        if "QCS6490" not in name or "Headset Jack" not in name:
            continue
        event = name_file.parents[1].name
        try:
            with open("/dev/input/" + event, "rb", buffering=0) as device:
                bits = bytearray(8)
                fcntl.ioctl(device.fileno(), 0x8008451b, bits, True)  # EVIOCGSW(8)
                mask = int.from_bytes(bits, "little")
            data["jack_switches"].append({"name": name, "event": event,
                "headphone": bool(mask & (1 << 2)), "lineout": bool(mask & (1 << 6)),
                "physical": bool(mask & (1 << 7))})
        except OSError as error:
            data["jack_switches"].append({"name": name, "error": str(error)})
    data["hashes"] = {p: digest(p) for p in (
        "/etc/card-defs.xml", "/usr/lib/libagm.so", "/usr/lib/libpal.so",
        "/lib/firmware/qcom/qcs6490/radxa/dragon-q6a/adsp.mbn")}
    data["processes"] = command(["ps", "-eo", "user,pid,args"])
    data["kernel_log"] = command(["dmesg"])
    uid = command(["id", "-u", user])["stdout"].strip()
    if uid.isdecimal():
        data["sinks"] = command(["runuser", "-u", user, "--", "env",
            f"XDG_RUNTIME_DIR=/run/user/{uid}", "pactl", "--format=json", "list", "sinks"])
    else:
        data["sinks"] = {"returncode": -1, "stdout": "", "stderr": "unknown desktop user"}
    return data


def issues(data):
    errors = []
    if data.get("schema_version") != 1:
        return ["unsupported report schema"]
    if not data.get("board"):
        errors.append("Q6A codec card missing")
    if "qcm6490virtualsndcard" not in data.get("virtual_cards", []):
        errors.append("wrong or missing QCS/QCM6490 virtual card definition")
    if not any(data.get("acdb", {}).values()):
        errors.append("no ACDB at the paths currently requested by AGM")
    configs = data.get("pal_configs", {})
    for kind in ("mixer_paths", "resourcemanager"):
        if not any(value for path, value in configs.items() if Path(path).name.startswith(kind + "_")):
            errors.append(f"missing Q6A {kind} configuration")
    try:
        sinks = json.loads(data["sinks"]["stdout"])
        usable = [s for s in sinks if s.get("name") != "auto_null" and
                  s.get("driver") not in ("module-null-sink", "module-null-sink.c") and
                  s.get("properties", {}).get("factory.name") != "support.null-audio-sink" and
                  s.get("properties", {}).get("q6a.jack.connected") != "false"]
        if data["sinks"]["returncode"] or not usable:
            errors.append("no non-null desktop output")
    except (KeyError, ValueError, TypeError):
        errors.append("desktop Pulse server unavailable")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse-user", default="weston")
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    if args.check:
        data = json.loads(args.check.read_text())
        errors = issues(data)
        print(json.dumps({"prerequisites_pass": not errors, "errors": errors,
                          "audible_acceptance": "not measured"}, indent=2))
        return bool(errors)
    print(json.dumps(collect(args.pulse_user), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
