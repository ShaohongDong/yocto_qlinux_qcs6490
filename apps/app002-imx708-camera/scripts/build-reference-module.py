#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build reference CHI module/actuator XML and binaries, without deploying them.

Uses externally supplied XSD/compiler. Optical values are from Raspberry Pi's
Camera Module 3 Wide specifications; actuator transport from its DW9817 driver.
The linear DAC region is an engineering range, NOT a calibrated focus curve.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from lxml import etree as ET


def add(parent, tag, text=None):
    node = ET.SubElement(parent, tag)
    if text is not None:
        node.text = str(text)
    return node


def fields(parent, values):
    for tag, text in values.items():
        add(parent, tag, text)


def root(tag, schema):
    node = ET.Element(tag, nsmap={"xsi": "http://www.w3.org/2001/XMLSchema-instance"})
    node.set("{http://www.w3.org/2001/XMLSchema-instance}noNamespaceSchemaLocation", str(schema))
    version = add(node, "module_version")
    version.attrib.update({"major_revision": "1", "minor_revision": "0", "incr_revision": "0"})
    return node


def setting(parent, address, value, width=1, operation="WRITE", delay=0):
    reg = add(parent, "regSetting")
    fields(reg, {"registerAddr": address, "registerData": value, "regAddrType": 1,
                 "regDataType": width, "operation": operation, "delayUs": delay})


def validate(node, schema):
    # Qualcomm's parser accepts a complex simple-content list item and xs:Float.
    # Normalize only these two vendor extensions for supplemental libxml2 checks;
    # ParameterParser still receives the original, unmodified vendor schemas.
    class VendorSchemaResolver(ET.Resolver):
        def resolve(self, url, pubid, context):
            path = Path(url)
            data = path.read_bytes()
            data = data.replace(b'itemType="RegisterData"', b'itemType="xs:unsignedInt"')
            data = data.replace(b'type="xs:Float"', b'type="xs:float"')
            return self.resolve_string(data, context, base_url=str(path))
    parser = ET.XMLParser(no_network=True)
    parser.resolvers.add(VendorSchemaResolver())
    ET.XMLSchema(ET.parse(str(schema), parser)).assertValid(node)


def module(schema):
    node = root("cameraModuleData", schema)
    cfg = add(add(node, "moduleGroup"), "moduleConfiguration")
    fields(cfg, {"cameraId": 0, "moduleName": "raspberrypi_cm3_wide",
                 "sensorName": "imx708_wide", "actuatorName": "dw9817",
                 "chromatixName": "imx708_wide", "position": "EXTERNAL"})
    fields(add(cfg, "CSIInfo"), {"laneAssign": 16, "isComboMode": 0})
    # maxFocusDistance representation is provisional: infinity must be checked
    # against the target CamX metadata interpretation, not used as an AF curve.
    fields(add(cfg, "lensInfo"), {"focalLength": 2.75, "fNumber": 2.2,
                                   "minFocusDistance": 0.05, "maxFocusDistance": "INF",
                                   "horizontalViewAngle": 102, "verticalViewAngle": 67})
    return node


def actuator(schema):
    node = root("actuatorDriver", schema)
    slave = add(node, "slaveInfo")
    fields(slave, {"actuatorName": "dw9817", "slaveAddress": 24,
                   "i2cFrequencyMode": "FAST", "actuatorType": "BIVCM", "dataBitWidth": 10})
    # VAF is the board's fixed 3.3 V module supply, not a bare-chip rail.
    for sequence in ("powerUpSequence", "powerDownSequence"):
        fields(add(add(slave, sequence), "powerSetting"),
               {"configType": "VAF", "configValue": 0, "delayMs": 12 if sequence == "powerUpSequence" else 0})
    config = add(node, "registerConfig")
    fields(add(config, "registerParam"), {
        "regAddrType": 1, "regDataType": 2, "registerAddr": 3, "registerData": 0,
        "operation": "WRITE_DAC_VALUE", "delayUs": 1000,
        "hwMask": 0, "hwShift": 0, "dataShift": 0})
    setting(add(node, "initSettings"), 2, 0)
    deinit = add(node, "deInitSettings")
    setting(deinit, 3, 512, width=2, delay=1000)
    setting(deinit, 2, 1)
    tuned = add(node, "tunedParams")
    add(tuned, "initialCode", 480)
    fields(add(add(tuned, "regionParams"), "region"), {
        "macroStepBoundary": 1023, "infinityStepBoundary": 0,
        "codePerStep": 1, "qValue": 1})
    for direction in ("forwardDamping", "backwardDamping"):
        damping = add(tuned, direction)
        add(damping, "ringingScenario", 1023)
        region = add(add(add(damping, "scenarioDampingParams"), "scenario"), "region")
        fields(region, {"dampingStep": 16, "dampingDelayUs": 1000, "hwParams": 0})
    return node


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cdk-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    cdk, out = args.cdk_root.resolve(), args.output.resolve()
    compiler = cdk / "tools/buildbins/linux64/ParameterParserGCC7"
    expected = "1aa4632f585cfcd939f61e34b6c820e7423020af9eacc5120272ed929363dbc5"
    if not compiler.is_file() or hashlib.sha256(compiler.read_bytes()).hexdigest() != expected:
        parser.error("Unreviewed reference compiler: expected the pinned 11se ParameterParserGCC7")
    manifest = json.loads((Path(__file__).resolve().parents[1] / "docs/chi-reference-inputs.json").read_text())
    for name, digest in manifest["files"].items():
        if name.endswith(".xsd"):
            path = cdk / name
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                parser.error(f"Unreviewed reference schema: {name}")
    if out.exists() and any(out.iterdir()):
        parser.error("Use an empty output directory to avoid mixing stale assets")
    out.mkdir(parents=True, exist_ok=True)
    artifacts = {}
    for name, schema_name, make in [
        ("imx708_wide_module", "camxmoduleconfig.xsd", module),
        ("dw9817_actuator", "camxactuatordriver.xsd", actuator),
    ]:
        schema = cdk / "api/sensor" / schema_name
        node = make(schema)
        validate(node, schema)
        xml = out / (name + ".xml")
        xml.write_bytes(ET.tostring(node, xml_declaration=True, encoding="UTF-8", pretty_print=True))
        binary = out / (name + ".bin")
        subprocess.run([str(compiler), str(binary), "b", str(xml)], check=True)
        if not binary.is_file() or not binary.stat().st_size:
            raise RuntimeError("Compiler produced no binary")
        artifacts[binary.name] = hashlib.sha256(binary.read_bytes()).hexdigest()
    report = {"status": "REFERENCE_FORMAT_ONLY", "artifacts": artifacts,
              "not_ready_for_deployment": [
                  "Target CamX binary reader compatibility is not yet verified",
                  "Sensor XML/register bundle and matching tuning remain to be integrated",
                  "DW9817 busy polling and gradual parking must be implemented by the CHI backend",
                  "DAC region is electrical range, not a calibrated AF mapping",
                  "Infinity metadata representation requires target verification",
                  "Power sequencing must coordinate sensor module enable and shared VAF supply",
              ]}
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
