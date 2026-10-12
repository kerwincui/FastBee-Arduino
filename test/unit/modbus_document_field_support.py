"""Frozen Modbus root-field oracle support; imported by existing real fixtures."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
ORACLE = ROOT / "test/unit/cpp/modbus_document_fields_before.inc"
FACTORY = ROOT / "include/utils/PsramJsonDocument.h"
EXPECTED_COUNTS = {
    "handleGetModbusStatus": 1, "handleModbusWrite": 2,
    "handleModbusRegisterWrite": 1, "handleModbusRegisterBatchWrite": 1,
    "handleModbusDeviceAddress": 1, "handleModbusDeviceBaudrate": 1,
    "handleModbusCoilControl": 4, "handleModbusCoilBatch": 1,
    "handleModbusCoilDelay": 2,
}


def inputs():
    return [Path(__file__), ORACLE, FACTORY,
            ROOT / "include/network/FixedJsonField.h",
            ROOT / "src/network/FixedJsonField.cpp"]


def definition(source, signature):
    if source.count(signature) != 1:
        raise AssertionError("Expected one method: " + signature)
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError("Unterminated method: " + signature)


def oracle_program(program, names):
    # The one auto-root in this batch is the factory's exact return type.
    if not re.search(r"inline\s+JsonDocument\s+makeJsonDocument\(size_t\s+psramReserveBytes",
                     FACTORY.read_text(encoding="utf-8")):
        raise AssertionError("Reviewed factory no longer returns exact JsonDocument")
    source = ORACLE.read_text(encoding="utf-8")
    for name in names:
        signature = "void ModbusRouteHandler::" + name + "("
        old = definition(source, signature)
        current = definition(program, signature)
        draft, count = re.subn(r'\b(doc|document|denied)\["([^"]+)"\]\s*=\s*([^;\n]*);',
            lambda match: 'FastBee::setFixedJsonField(' + match[1] + ', "' +
                match[2] + '", ' + match[3] + ');', old)
        if count != EXPECTED_COUNTS[name] or current != draft:
            raise AssertionError("Exact integrated document field body required: " + name)
        program = program.replace(current, old, 1)
    return program
