"""Disable only mDNS in a freshly backed-up COM4/COM16 filesystem.

Application firmware is never written. Hardware access requires --write, a
live inherited repository lock, successful local gates, and exact known
application/partition/device identity. All private data stays in local files.
"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time

from littlefs import LittleFS

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    'COM4': ('ESP32-C3', '146393902f64', 'com4-deployment-r6.json',
             '6f41c3969bec8432f0b0a2a6b897fd892488ef2833ddca8e1dcbff09cb2bedba'),
    'COM16': ('ESP32', 'd4e9f4b16c34', 'com16-deployment-r5.json',
              '0f331cb921b3d9effbbdd0fb8a838f0db1df9a97ad8b2ac499820bf91ba75d04'),
}
CONFIG_KEYS = {'config/network.json': ('enableMDNS',),
               'config/features.json': ('features', 'mdns', 'enabled')}
FLASH_BYTES, FS_OFFSET, FS_BYTES = 0x400000, 0x340000, 0xC0000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def save_new(path, value):
    with Path(path).open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)
        stream.write('\n')


def json_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON keys are not permitted')
        result[key] = value
    return result


def disabled_configs(contents):
    result, changes = dict(contents), []
    for path, keys in CONFIG_KEYS.items():
        require(path in contents, 'Required mDNS configuration file missing')
        original = json.loads(contents[path].decode('utf-8-sig'), object_pairs_hook=json_object)
        updated = copy.deepcopy(original)
        cursor = updated
        for key in keys[:-1]:
            require(isinstance(cursor, dict) and isinstance(cursor.get(key), dict),
                    'Unsupported mDNS configuration schema')
            cursor = cursor[key]
        require(isinstance(cursor, dict) and type(cursor.get(keys[-1])) is bool,
                'The mDNS setting must be an existing boolean')
        if cursor[keys[-1]] is False:
            continue
        cursor[keys[-1]] = False
        result[path] = (json.dumps(updated, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
        # Restore the sole changed value and prove every other JSON field equal.
        restored = copy.deepcopy(updated)
        cursor = restored
        for key in keys[:-1]:
            cursor = cursor[key]
        cursor[keys[-1]] = True
        require(restored == original, 'Unexpected non-mDNS configuration change')
        changes.append(path)
    return result, changes


def patch_image(image, helper):
    contents, directories = helper.read_files(image)
    desired, changed = disabled_configs(contents)
    if not changed:
        return image, dict(changedFiles=[], preservedFileCount=len(contents),
                           noOp=True, verified=True, mdnsEnabled=False)
    fs = LittleFS(block_size=4096, block_count=len(image)//4096,
                  read_size=1, prog_size=1, cache_size=4096,
                  lookahead_size=32, block_cycles=500, name_max=64,
                  disk_version=0x00020001, mount=False)
    fs.context.buffer = bytearray(image)
    fs.mount()  # Never format or replace the existing filesystem.
    try:
        for path in changed:
            with fs.open('/' + path, 'wb') as stream:
                require(stream.write(desired[path]) == len(desired[path]), 'Incomplete configuration write')
    finally:
        fs.unmount()
    candidate = bytes(fs.context.buffer)
    actual, after_dirs = helper.read_files(candidate)
    require(actual == desired and after_dirs == directories,
            'Filesystem roundtrip altered an unrelated file or directory')
    require(len(candidate) == len(image), 'Filesystem size changed')
    return candidate, dict(changedFiles=changed, preservedFileCount=len(contents)-len(changed),
                           preservedDirectoryCount=len(directories), noOp=False, verified=True,
                           mdnsEnabled=False, files=[dict(path=p, bytes=len(data), sha256=digest(data))
                                                    for p, data in sorted(actual.items())])


def require_lock():
    owner_path = ROOT / '.pio/locks/firmware-build.owner.json'
    owner = json.loads(owner_path.read_text(encoding='utf-8-sig'))
    require(os.environ.get('FASTBEE_FIRMWARE_LOCK_TOKEN') == owner.get('token') and
            bool(owner.get('token')), 'No matching inherited shared lock')
    require(Path(os.environ.get('FASTBEE_FIRMWARE_LOCK_ROOT', '')).resolve() == ROOT.resolve(),
            'Wrong repository lock root')
    require(Path(owner['projectRoot']).resolve() == ROOT.resolve() and
            str(owner['pid']) == os.environ.get('FASTBEE_FIRMWARE_LOCK_OWNER_PID') and
            str(owner['processStartTicks']) == os.environ.get('FASTBEE_FIRMWARE_LOCK_OWNER_STARTED_TICKS'),
            'Inherited shared lock owner identity mismatch')
    pid = int(owner['pid'])
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                             f'(Get-Process -Id {pid} -ErrorAction Stop).StartTime.ToUniversalTime().Ticks'],
                            capture_output=True, text=True, timeout=15)
    require(result.returncode == 0 and result.stdout.strip() == str(owner['processStartTicks']),
            'Shared lock owner is no longer the recorded process')


def verify_write_gate(report):
    require(report.get('identityVerified') is True and report.get('backupVerified') is True and
            report.get('applicationVerified') is True and report.get('sourceStable') is True and
            report.get('candidateVerified') is True, 'A required pre-write gate is missing')


def read_flash_block(esp, offset, length, port):
    """Same repository 1KiB/one-frame transport, with precise failure evidence.

    No retries, timeouts relaxed, or corrupt-frame acceptance. Payload contents
    are never included in diagnostics; complete blocks get two MD5 checks.
    """
    esp.check_command('read flash', esp.ESP_CMDS['READ_FLASH'],
                      struct.pack('<IIII', offset, length, 1024, 1))
    content=bytearray()
    while len(content)<length:
        esp._port.timeout=5
        packet=esp.read()
        expected=min(1024,length-len(content))
        require(len(packet)==expected,
                f'Flash frame length mismatch at 0x{offset+len(content):x}: expected {expected}, received {len(packet)}; no flash write allowed')
        content.extend(packet); esp.write(struct.pack('<I',len(content)))
    frame=esp.read()
    require(len(frame)==16 and hashlib.md5(content).digest()==frame,'Streamed flash MD5 mismatch')
    require(esp.flash_md5sum(offset,length).lower()==hashlib.md5(content).hexdigest(),'Independent flash MD5 mismatch')
    print(f'{port}: verified backup block 0x{offset:x}, {length} bytes',flush=True)
    return bytes(content)


def capture_after_reset(esp, output, seconds, report):
    # Reuse the verified device handle; no DTR/RTS reopen or serial commands.
    esp._port.baudrate = 115200
    esp._port.timeout = 0.25
    result = dict(startedAt=now(), durationSeconds=seconds, serialWrites=0, resetCount=0)
    report['resetRequested'] = True
    esp.hard_reset()
    report['resetCompleted'] = True
    result['resetCount'] = 1
    deadline = time.monotonic() + seconds
    with (output/'post-disable-serial.log').open('xb') as raw, \
            (output/'post-disable-chunks.jsonl').open('x', encoding='utf-8') as chunks:
        while time.monotonic() < deadline:
            data = esp._port.read(4096)
            if data:
                raw.write(data)
                raw.flush()
                chunks.write(json.dumps(dict(at=now(), text=data.decode('utf-8', errors='replace')),
                                        ensure_ascii=False) + '\n')
                chunks.flush()
    result.update(finishedAt=now(), state='COMPLETED')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', choices=tuple(TARGETS), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gates', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--capture-seconds', type=int, default=150)
    args = parser.parse_args()
    require(args.write, 'Explicit --write authorization is required; no implicit device reset')
    require(60 <= args.capture_seconds <= 300, 'Capture duration outside bounded limits')
    require_lock()
    output = args.output.resolve()
    require(not output.exists(), 'Existing evidence must not be overwritten')
    gates = json.loads(args.gates.read_text(encoding='utf-8-sig'))
    helper_path = ROOT/'artifacts/deploy-live-preserved-web.py'
    helper = load_module('mdns_preserved_deploy_helpers', helper_path)
    inputs = {str(Path(__file__).resolve()): digest(Path(__file__).read_bytes()),
              str(helper_path.resolve()): digest(helper_path.read_bytes())}
    require(gates.get('passed') is True and gates.get('sourceHashes') == inputs,
            'Successful regression/source-stability gate required')
    chip, expected_mac, baseline_name, expected_sha = TARGETS[args.port]
    baseline = json.loads((ROOT/'artifacts/com4-com16-20260929/repair-20260930'/baseline_name).read_text(encoding='utf-8-sig'))
    require(baseline.get('written') is True and baseline.get('verified') is True and
            baseline.get('firmwareSha256') == expected_sha, 'Unexpected deployment baseline')
    build_path = Path(baseline['buildReport'])
    release = build_path.parent / baseline['environment']
    firmware = (release/'firmware.bin').read_bytes()
    require(digest(firmware) == expected_sha, 'Original firmware archive hash changed')
    helper.select_build_report(json.loads(build_path.read_text(encoding='utf-8-sig')),
                               args.port, baseline['environment'], expected_sha)
    table = (release/'partitions.bin').read_bytes()
    parts = helper.parse_partitions(table, FLASH_BYTES)
    fs_parts = [p for p in parts if p['type'] == 1 and p['name'] in ('spiffs', 'littlefs')]
    require(len(fs_parts) == 1 and (fs_parts[0]['offset'], fs_parts[0]['size']) == (FS_OFFSET, FS_BYTES),
            'Unexpected filesystem geometry')
    output.mkdir(parents=True, exist_ok=False)
    report = dict(startedAt=now(), port=args.port, mode='MDNS_CONFIG_ONLY', phase='filesystem',
                  outputDirectory=str(output), requestedWrite=True, written=False, verified=False,
                  applicationWritten=False, filesystemWritten=False,
                  firmwareSha256=expected_sha, filesystemOffset=FS_OFFSET,
                  filesystemBytes=FS_BYTES, resetRequested=False, sourceHashes=inputs)
    esp = None
    sys.path.insert(0, 'C:/Users/kerwin/.platformio/packages/tool-esptoolpy')
    try:
        from esptool.cmds import detect_chip, attach_flash, write_flash
        esp = detect_chip(port=args.port)
        require(esp.CHIP_NAME == chip and bytes(esp.read_mac('BASE_MAC')).hex() == expected_mac,
                'Device chip/MAC mismatch')
        report['identityVerified'] = True
        esp = esp.run_stub()
        attach_flash(esp)
        require((1 << ((esp.flash_id() >> 16) & 255)) == FLASH_BYTES, 'Physical flash capacity mismatch')
        esp.flash_set_parameters(FLASH_BYTES)
        esp.change_baud(115200)
        require(esp.flash_md5sum(0x8000, len(table)).lower() == helper.md5(table), 'Partition table differs')
        require(esp.flash_md5sum(0x10000, len(firmware)).lower() == helper.md5(firmware),
                'Current firmware is not the verified r6/r5 baseline')
        report['applicationVerified'] = True
        report['protectedPrefixMd5Before'] = esp.flash_md5sum(0, FS_OFFSET).lower()
        chunks=[]
        for offset in range(0, FLASH_BYTES, 0x40000):
            report['backupNextOffset']=offset
            block=read_flash_block(esp,offset,0x40000,args.port)
            with (output/f'backup-block-{offset:06x}.bin').open('xb') as stream:
                stream.write(block)
            chunks.append(block)
        snapshot = b''.join(chunks)
        require(esp.flash_md5sum(0, FLASH_BYTES).lower() == helper.md5(snapshot), 'Full backup MD5 mismatch')
        with (output/'full-flash-before.bin').open('xb') as stream:
            stream.write(snapshot)
        report.update(backupVerified=True, backupSha256=digest(snapshot), backupMd5=helper.md5(snapshot))
        ota = next(p for p in parts if p['type'] == 1 and p['subtype'] == 0)
        report['otaSelection'] = helper.verify_active_app_slot(snapshot[ota['offset']:ota['offset']+ota['size']], parts, 0)
        original = snapshot[FS_OFFSET:FS_OFFSET+FS_BYTES]
        report['filesystemMd5Before'] = helper.md5(original)
        candidate, manifest = patch_image(original, helper)
        save_new(output/'configuration-change-manifest.json', manifest)
        with (output/'filesystem-mdns-disabled.bin').open('xb') as stream:
            stream.write(candidate)
        report.update(candidateVerified=True, configurationManifest='configuration-change-manifest.json',
                      filesystemSha256=digest(candidate), changedFiles=manifest['changedFiles'],
                      unchangedApplication=True, preservedFileCount=manifest['preservedFileCount'])
        current_inputs = {path: digest(Path(path).read_bytes()) for path in inputs}
        report['sourceStable'] = current_inputs == inputs
        verify_write_gate(report)
        require_lock()
        require(esp.flash_md5sum(FS_OFFSET, FS_BYTES).lower() == helper.md5(original), 'Pre-write filesystem changed')
        save_new(output/'prewrite-gates.json', report)
        if not manifest['noOp']:
            report['writeStarted'] = True
            write_flash(esp, [(FS_OFFSET, candidate)], compress=True, no_progress=True)
            report['written'] = True
            report['filesystemWritten'] = True
        else:
            report['noOpAlreadyDisabled'] = True
        report['filesystemMd5After'] = esp.flash_md5sum(FS_OFFSET, FS_BYTES).lower()
        report['protectedPrefixMd5After'] = esp.flash_md5sum(0, FS_OFFSET).lower()
        require(report['filesystemMd5After'] == helper.md5(candidate), 'Written filesystem MD5 mismatch')
        require(report['protectedPrefixMd5After'] == report['protectedPrefixMd5Before'],
                'Non-filesystem flash was unexpectedly altered')
        report['verified'] = True
        report['postResetCapture'] = capture_after_reset(esp, output, args.capture_seconds, report)
        report['deviceEffect'] = 'AWAITING_SANITIZED_SERIAL_REVIEW'
    except Exception as error:
        report.update(errorType=type(error).__name__, error=str(error))
        raise
    finally:
        if esp is not None:
            try:
                if not report['resetRequested'] and not (report.get('writeStarted') and not report['verified']):
                    report['resetRequested'] = True
                    esp.hard_reset()
                    report['resetCompleted'] = True
                elif report.get('writeStarted') and not report['verified']:
                    report['recoveryRequired'] = True
            finally:
                esp._port.close()
                report['portClosed'] = True
        report['finishedAt'] = now()
        save_new(output/'deployment.json', report)
    print(json.dumps({key: report.get(key) for key in ('port','written','verified','applicationWritten',
                     'backupVerified','sourceStable','changedFiles','portClosed')}, ensure_ascii=True), flush=True)


if __name__ == '__main__':
    main()
