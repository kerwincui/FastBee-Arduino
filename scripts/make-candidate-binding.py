"""Generate a monitor-device-candidate binding for one deployed device.

Post-deploy only: logs in once (FASTBEE_PASSWORD env), reads system info and
every immutable config document, and freezes their fingerprints plus the
deployment evidence into a private binding.json. Prints nothing sensitive.

Contract source: scripts/monitor-device-candidate.py validates the private
binding and requires every deploymentEvidence entry to carry identityPath /
identitySha256 pointing at an identity.json with mac, port,
deploymentReportSha256 and sourceSha256; a binding without identity evidence
fails its Binding checks (KeyError 'identityPath'). This script is the
maintained generator for that contract, replacing ad-hoc copies under tmp/.
Config fingerprints reuse the monitor's own json_data / config_fingerprint /
CONFIG_NAMES / REQUIRED_CONFIGS / CONFIG_API_VIEWS / mac / digest helpers via
a normal same-directory import, so the fingerprint rules cannot drift apart.

binding.json and identity.json are written with exclusive 'x' mode: existing
evidence files are never overwritten; rerun with a fresh --out directory.

Example (run after a verified deployment; password only via environment):

    $env:FASTBEE_PASSWORD = '<device-password>'
    python scripts/make-candidate-binding.py `
      --device-id <device-id> --mac <measured-mac> `
      --url http://<device-sta-address> --port-name <serial-port> `
      --deployment-report <deployment.json> `
      --source-sha256 <64-hex> --web-sha256 <64-hex> `
      --out <private-dir>/binding.json
"""
import argparse
import importlib
import json
import os
from pathlib import Path

monitor = importlib.import_module('monitor-device-candidate')

import requests

parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--device-id', required=True)
parser.add_argument('--mac', required=True)
parser.add_argument('--url', required=True)
parser.add_argument('--port-name', required=True)
parser.add_argument('--deployment-report', required=True,
                    help='phase=all deployment report, or the firmware-phase report when '
                         '--filesystem-report is given (screen-variant app-slot deploys)')
parser.add_argument('--filesystem-report',
                    help='optional separate phase=filesystem deployment report; combined with '
                         '--deployment-report as firmware+filesystem evidence entries')
parser.add_argument('--source-sha256', required=True)
parser.add_argument('--web-sha256', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()

password = os.environ.get('FASTBEE_PASSWORD')
if not password:
    raise SystemExit('FASTBEE_PASSWORD required')
origin = args.url.rstrip('/')
session = requests.Session()
reply = session.post(origin + '/api/auth/login',
                     data={'username': os.environ.get('FASTBEE_USERNAME', 'admin'), 'password': password},
                     timeout=15)
reply.raise_for_status()
login = reply.json()
if login.get('success', True) is False:
    raise SystemExit('login failed')

info = monitor.json_data(session.get(origin + '/api/system/info?full=1', timeout=15).content)
if monitor.mac(info.get('network', {}).get('macAddress')) != monitor.mac(args.mac):
    raise SystemExit('identity mismatch')

configs = []
for name in sorted(monitor.CONFIG_NAMES):
    route = monitor.CONFIG_API_VIEWS.get(name, '/api/config/transfer/export?name=' + name)
    response = session.get(origin + route, timeout=15)
    if response.status_code == 404:
        continue
    response.raise_for_status()
    body = response.content
    try:
        fingerprint = monitor.config_fingerprint(name, body)
    except Exception:
        if name in monitor.REQUIRED_CONFIGS:
            raise
        continue
    configs.append(dict(name=name, sha256=fingerprint))
names = {item['name'] for item in configs}
if not monitor.REQUIRED_CONFIGS.issubset(names):
    raise SystemExit('missing required configs: ' + ','.join(sorted(monitor.REQUIRED_CONFIGS - names)))

deployment = Path(args.deployment_report).resolve()
report = json.loads(deployment.read_text(encoding='utf-8-sig'))
if not (report.get('written') is True and report.get('verified') is True):
    raise SystemExit('deployment not verified')

filesystem = None
fs_report = None
if args.filesystem_report:
    # Two-report mode: --deployment-report is the firmware-phase evidence (screen-variant
    # app-slot reports carry no phase field), --filesystem-report the filesystem phase.
    if report.get('phase') is None and not (
            report.get('schemaVersion') == 2 and type(report.get('appSlot')) is int
            and report.get('filesystemWritten') is False):
        raise SystemExit('firmware report missing phase')
    filesystem = Path(args.filesystem_report).resolve()
    fs_report = json.loads(filesystem.read_text(encoding='utf-8-sig'))
    if not (fs_report.get('written') is True and fs_report.get('verified') is True):
        raise SystemExit('filesystem deployment not verified')
    if fs_report.get('phase') != 'filesystem':
        raise SystemExit('filesystem report phase mismatch')
    if fs_report.get('firmwareSha256') != report['firmwareSha256']:
        raise SystemExit('firmware hash mismatch between reports')
    if fs_report.get('port') != args.port_name:
        raise SystemExit('filesystem report port mismatch')
    if fs_report.get('webSha256') != args.web_sha256:
        raise SystemExit('filesystem report web hash mismatch')
    if not monitor.SHA.fullmatch(str(fs_report.get('filesystemSha256', ''))):
        raise SystemExit('filesystem report missing filesystemSha256')

out = Path(args.out).resolve()
# 输出目录可能不存在（新批次目录）；排他 'x' 写入仍需父目录在场。
out.parent.mkdir(parents=True, exist_ok=True)


def write_identity(suffix, report_sha):
    identity = dict(mac=args.mac, port=args.port_name,
                    deploymentReportSha256=report_sha, sourceSha256=args.source_sha256)
    identity_path = out.parent / ('identity%s.json' % suffix)
    with identity_path.open('x', encoding='utf-8') as stream:
        json.dump(identity, stream, indent=2)
        stream.write('\n')
    return identity_path


def evidence_entry(phase, report_path, identity_path):
    return dict(phase=phase,
                path=os.path.relpath(report_path, out.parent).replace('\\', '/'),
                sha256=monitor.digest(report_path.read_bytes()),
                identityPath=identity_path.name,
                identitySha256=monitor.digest(identity_path.read_bytes()))


if fs_report is None:
    evidence = [evidence_entry('all', deployment, write_identity('', monitor.digest(deployment.read_bytes())))]
else:
    evidence = [
        evidence_entry('firmware', deployment,
                       write_identity('-firmware', monitor.digest(deployment.read_bytes()))),
        evidence_entry('filesystem', filesystem,
                       write_identity('-filesystem', monitor.digest(filesystem.read_bytes())))]

binding = dict(
    schemaVersion=1,
    deviceId=args.device_id,
    mac=args.mac,
    url=origin,
    ports=[args.port_name],
    sourceSha256=args.source_sha256,
    firmwareSha256=report['firmwareSha256'],
    webSha256=args.web_sha256,
    firmwareVersion=info['device']['firmwareVersion'],
    sketchSize=info['flash']['sketchSize'],
    hasPsram=bool(info.get('memory', {}).get('psram', {}) or info.get('psram')),
    intervals=dict(health=30, resources=60, page=600, config=600),
    configs=configs,
    deploymentEvidence=evidence)
with out.open('x', encoding='utf-8') as stream:
    json.dump(binding, stream, indent=2)
    stream.write('\n')
print(json.dumps(dict(deviceId=args.device_id, binding=str(out), configCount=len(configs),
                      firmwareVersion=binding['firmwareVersion'], sketchSize=binding['sketchSize'])))
