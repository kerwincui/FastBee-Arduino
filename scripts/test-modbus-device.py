"""COM15 acceptance: read-only by default; explicit relay exercise restores raw state.

Credentials come from FASTBEE_USERNAME / FASTBEE_PASSWORD and are never saved.
Use --expected-mac to prevent operating another device after an IP change.
Physical sensor accuracy and electrical edge latency require independent measurements.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import struct
import threading
import time


def require(condition, message):
    if not condition:
        raise ValueError(message)


def request_api(client, base, route, params, method, report):
    """Keep transport phases and a safe peripheral ID without storing credentials."""
    started = time.monotonic()
    observation = dict(route=route.split('?', 1)[0], method=method, status=None,
                       at=datetime.now(timezone.utc).isoformat(), failurePhase=None)
    if (observation['route'] == '/api/peripherals/read' and isinstance(params, dict) and
            isinstance(params.get('id'), str) and re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', params['id'])):
        observation['peripheralId'] = params['id']
    response = None
    phase = 'response_headers'
    try:
        # Streaming exposes the received status even when reading the body fails.
        response = client.request(method, base + route,
                                  params=params if method == 'GET' else None,
                                  data=params if method == 'POST' else None,
                                  timeout=(3, 12), stream=True,
                                  headers={'Accept-Encoding': 'identity'})
        observation['status'] = response.status_code
        phase = 'http_response'
        response.raise_for_status()
        phase = 'response_body'
        raw = response.content
        observation['bodyBytes'] = len(raw)
        declared = response.headers.get('Content-Length')
        if declared is not None:
            require(isinstance(declared, str) and declared.isdecimal() and len(raw) == int(declared),
                    'Incomplete HTTP response body: ' + observation['route'])
        observation['bodyComplete'] = True
        phase = 'response_json'
        body = response.json()
        require(isinstance(body, dict) and body.get('success') is True,
                'Request failed: ' + observation['route'])
        return body.get('data') or {}
    except Exception as error:
        observation.update(error=type(error).__name__, failurePhase=phase)
        raise
    finally:
        observation['elapsedMs'] = round((time.monotonic() - started) * 1000)
        report['requests'].append(observation)
        if response is not None:
            response.close()


def decode_mapping(task, mapping):
    cache = task.get('cachedData') or {}
    require(task.get('status') == 'ok' and cache.get('valid', True) is not False
            and not task.get('lastError') and not cache.get('lastError'), 'Sample is missing, stale or failed')
    words = cache.get('values', [])
    offset, kind = mapping.get('regOffset', 0), mapping.get('dataType', 0)
    require(type(offset) is int and offset >= 0 and type(kind) is int and 0 <= kind <= 4, 'Invalid mapping')
    count = 2 if kind in (2, 3, 4) else 1
    order = mapping.get('wordOrder', 0)
    require(type(order) is int and order in (0, 1) and (count == 2 or order == 0), 'Invalid word order')
    require(isinstance(words, list) and offset + count <= len(words), 'Incomplete register sample')
    selected = words[offset:offset + count]
    require(all(type(w) is int and 0 <= w <= 65535 for w in selected), 'Invalid register word')
    raw = selected[0]
    if kind == 1 and raw >= 32768:
        raw -= 65536
    if count == 2:
        bits = (selected[order] << 16) | selected[1 - order]
        raw = struct.unpack('>f', struct.pack('>I', bits))[0] if kind == 4 else bits
        if kind == 3 and bits >= 2147483648:
            raw -= 4294967296
    scale = mapping.get('scaleFactor', 1)
    require(type(scale) in (int, float) and math.isfinite(scale) and abs(scale) <= 3.4028234663852886e38, 'Invalid scale factor')
    stored_scale = struct.unpack('>f', struct.pack('>f', scale))[0]
    require(scale == 0 or stored_scale != 0, 'Scale factor underflow')
    scale = stored_scale
    value = raw * scale
    require(math.isfinite(value), 'Non-finite reading')
    return value


def mapped_readings(status):
    readings, errors = [], []
    for task in status.get('tasks', []):
        if not task.get('enabled'):
            continue
        for mapping in task.get('mappings', []):
            if not mapping.get('sensorId'):
                continue
            try:
                readings.append(dict(id=mapping['sensorId'], slave=task.get('slaveAddress'),
                                     value=decode_mapping(task, mapping), unit=mapping.get('unit', '')))
            except ValueError as error:
                errors.append(dict(id=mapping['sensorId'], reason=str(error)))
    require(readings or errors, 'No enabled mapped sensor channels')
    return readings, errors


def validate_states(data, count):
    states = data.get('states')
    require(isinstance(states, list) and len(states) == count and
            all(type(value) is bool for value in states), 'Incomplete or unknown relay states')
    return states


def verify_uptime(previous, current):
    require(type(current) in (int, float) and math.isfinite(current), 'Uptime is missing')
    require(previous is None or current >= previous, 'Device restarted: uptime decreased')
    return current


def sampling_failures(report):
    failures = []
    samples = report['samples']
    if not samples:
        failures.append('No complete monitoring samples')
    if any(row.get('mappingErrors') for row in samples):
        failures.append('Some mapped sensors have no valid reading during the observation window')
    if report.get('transportErrors'):
        failures.append('Transport errors occurred during acceptance')
    if report.get('diagnosticErrors'):
        failures.append('Some diagnostic requests failed')
    if report.get('samplingErrors'):
        failures.append('Some monitoring samples were unavailable or incomplete')
    if report.get('relay', {}).get('status') == 'FAIL':
        failures.append('Relay acceptance failed: ' + report['relay']['error'])
    initial, final = report.get('initialModbus', {}), report.get('finalModbus', {})
    intervals = [task.get('pollInterval') for task in initial.get('tasks', []) if task.get('enabled')]
    intervals = [value for value in intervals if type(value) in (int, float) and value > 0]
    if intervals and len(samples) > 1 and all(type(row.get('uptime')) in (int, float)
                                             for row in (samples[0], samples[-1])):
        elapsed = samples[-1]['uptime'] - samples[0]['uptime']
        counts = initial.get('totalPolls'), final.get('totalPolls')
        if elapsed >= max(intervals) + 5 and all(type(count) is int for count in counts):
            if counts[1] <= counts[0]:
                failures.append('Background polling did not advance across the configured interval')
    return failures


def capture_diagnostics(api, report, phase):
    snapshot = {}
    report[phase + 'Diagnostics'] = snapshot
    for route, key in [('/api/system/diag-bundle', 'system'),
                       ('/api/system/web-runtime?full=1', 'web'),
                       ('/api/peripherals/read?id=ai_soil_temp', 'soilTemperature'),
                       ('/api/peripherals/read?id=ai_soil_humidity', 'soilHumidity')]:
        try:
            data = api(route)
            # Do not archive network/authentication settings from the bundle.
            if key == 'system':
                snapshot[key] = {name: data.get(name) for name in ('restart', 'memory')}
            elif key == 'web':
                snapshot[key] = dict(sseStats=data.get('web', {}).get('sseStats'),
                                     softRestartCount=data.get('recovery', {}).get('softRestartCount'))
            else:
                snapshot[key] = data
        except Exception as error:
            report.setdefault('diagnosticErrors', []).append(dict(phase=phase, route=route, error=str(error)))


def probe_relay(api, args, report):
    # A failed bus probe must not stop uptime/serial monitoring. Never operate
    # an output unless exercise_relay has obtained every initial channel state.
    report.update(status='FAIL', mode='exercise' if args.exercise_relay else 'read-only')
    try:
        config = api('/api/protocol/config', dict(section='modbusRtu'))
        devices = config.get('modbusRtu', {}).get('master', {}).get('devices', [])
        selected = [d for d in devices if d.get('enabled') and d.get('deviceType') == 'relay'
                    and d.get('slaveAddress') == args.relay_slave]
        require(len(selected) == 1, 'Expected one configured relay at requested slave')
        device = selected[0]
        require(device.get('controlProtocol', 0) in (0, 1), 'Unknown relay control protocol')
        params = dict(slaveAddress=args.relay_slave, channelCount=device['channelCount'],
                      coilBase=device.get('coilBase', 0),
                      mode='register' if device.get('controlProtocol') == 1 else 'coil')
        report['parameters'] = params
        if args.exercise_relay:
            exercise_relay(api, params, args.channel, args.cycles, report)
        else:
            report['states'] = validate_states(api('/api/modbus/coil/status', params=params),
                                              params['channelCount'])
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)


def exercise_relay(api, params, channel, cycles, report):
    """One actuator command at a time, concurrent health read, always restore."""
    initial = validate_states(api('/api/modbus/coil/status', params=params), params['channelCount'])
    require(0 <= channel < len(initial), 'Relay channel outside configured device')
    report.update(initialStates=initial, restored=False, operations=[])
    command = {key: value for key, value in params.items() if key != 'channelCount'}
    command['channel'] = channel
    try:
        with ThreadPoolExecutor(max_workers=1) as pool:
            previous_uptime = None
            for index in range(cycles):
                target = not initial[channel] if index % 2 == 0 else initial[channel]
                future = pool.submit(api, '/api/system/info')
                started = time.monotonic()
                api('/api/modbus/coil/control', method='POST', params=dict(command, action='on' if target else 'off'))
                command_ms = round((time.monotonic() - started) * 1000)
                states = validate_states(api('/api/modbus/coil/status', params=params), params['channelCount'])
                expected = initial.copy(); expected[channel] = target
                require(states == expected, 'Relay readback differs or another channel changed')
                info = future.result()
                previous_uptime = verify_uptime(previous_uptime, info.get('uptime', {}).get('seconds'))
                report['operations'].append(dict(index=index, target=target, readback=states,
                                                 commandMs=command_ms,
                                                 elapsedMs=round((time.monotonic() - started) * 1000)))
                time.sleep(0.2)
    finally:
        restored = validate_states(api('/api/modbus/coil/status', params=params), params['channelCount'])
        if restored[channel] != initial[channel]:
            api('/api/modbus/coil/control', method='POST',
                params=dict(command, action='on' if initial[channel] else 'off'))
            restored = validate_states(api('/api/modbus/coil/status', params=params), params['channelCount'])
        report['restored'] = restored == initial
        require(report['restored'], 'Relay restoration failed; check device state')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--expected-mac', required=True)
    parser.add_argument('--port', help='Capture serial without pulsing DTR/RTS')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--exercise-relay', action='store_true')
    parser.add_argument('--relay-slave', type=int, default=6)
    parser.add_argument('--channel', type=int, default=0)
    parser.add_argument('--cycles', type=int, default=4)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    require(1 <= args.seconds <= 3600 and 1 <= args.cycles <= 20, 'Invalid duration/cycle bound')
    out = Path(args.out); out.mkdir(parents=True, exist_ok=False)
    report = dict(at=datetime.now(timezone.utc).isoformat(), status='BLOCKED', samples=[], requests=[],
                  relayExerciseRequested=args.exercise_relay, unverified=['physical-input-to-output-latency',
                  'sensor-calibration', 'long-duration-soak'])
    stop = threading.Event(); serial_thread = None; stream = None
    try:
        if args.port:
            from serial.tools import list_ports
            report['availablePorts'] = [port.device for port in list_ports.comports()]
            require(args.port.upper() in [p.upper() for p in report['availablePorts']],
                    'Requested serial port is not connected: ' + args.port)
        import requests
        session = requests.Session(); session.trust_env = False
        username = os.environ.get('FASTBEE_USERNAME', 'admin')
        password = os.environ.get('FASTBEE_PASSWORD')
        require(password is not None, 'Set FASTBEE_PASSWORD before running acceptance')
        response = session.post(args.url.rstrip('/') + '/api/auth/login',
                                data=dict(username=username, password=password), timeout=(3, 12))
        response.raise_for_status(); require(response.json().get('success') is True, 'Login failed')
        cookies = session.cookies.get_dict()
        def api(route, params=None, method='GET'):
            # Separate clients allow real overlapping health/control requests without sharing a Session.
            with requests.Session() as client:
                client.trust_env = False; client.cookies.update(cookies)
                return request_api(client, args.url.rstrip('/'), route, params, method, report)
        info = api('/api/system/info')
        normalize = lambda text: re.sub('[^0-9a-f]', '', text.lower())
        require(normalize(info.get('network', {}).get('macAddress', '')) == normalize(args.expected_mac)
                and len(normalize(args.expected_mac)) == 12, 'Device MAC does not match')
        require(info.get('device', {}).get('chipModel') == 'ESP32-S3', 'Expected an ESP32-S3')
        previous = verify_uptime(None, info.get('uptime', {}).get('seconds'))
        report['initialUptime'] = previous
        if args.port:
            import serial
            stream = serial.Serial(port=None, baudrate=115200, timeout=0.2)
            stream.dtr = False; stream.rts = False; stream.port = args.port; stream.open()
            def capture():
                try:
                    with (out / 'serial.log').open('wb') as log:
                        while not stop.is_set():
                            data = stream.read(max(1, stream.in_waiting))
                            if data: log.write(data); log.flush()
                except Exception as error:
                    report['serialError'] = str(error)
            serial_thread = threading.Thread(target=capture); serial_thread.start()
        report['status'] = 'FAIL'
        capture_diagnostics(api, report, 'initial')
        report['relay'] = {}
        probe_relay(api, args, report['relay'])
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            try:
                info = api('/api/system/info')
                previous = verify_uptime(previous, info.get('uptime', {}).get('seconds'))
                modbus = api('/api/modbus/status', dict(full=1))
                report.setdefault('initialModbus', modbus)
                report['finalModbus'] = modbus
                try:
                    readings, errors = mapped_readings(modbus)
                except ValueError as error:
                    # Memory recovery can temporarily stop the protocol. Keep
                    # observing uptime and restoration without treating it as empty success.
                    readings, errors = [], [dict(id='modbus', reason=str(error))]
                    report.setdefault('samplingErrors', []).append(dict(uptime=previous, error=str(error)))
                row = dict(uptime=previous, readings=readings, mappingErrors=errors,
                           totalPolls=modbus.get('totalPolls'),
                           lastPollAgeSec=modbus.get('health', {}).get('lastPollAgeSec'))
                report['samples'].append(row)
                for ident in ['di1', 'di2', 'do1']:
                    state = api('/api/peripherals/read', dict(id=ident)).get('state')
                    require(type(state) in (bool, int) and state in (0, 1), 'Missing digital input/output: ' + ident)
                    row[ident] = state
                print(json.dumps(dict(uptime=previous, mappedChannels=len(readings), invalidChannels=len(errors))), flush=True)
            except requests.RequestException as error:
                # Keep capturing serial and later uptime through transport loss.
                # Recovery is evidence, not an overall acceptance pass.
                report.setdefault('transportErrors', []).append(dict(
                    at=datetime.now(timezone.utc).isoformat(), error=str(error),
                    type=type(error).__name__,
                    status=getattr(getattr(error, 'response', None), 'status_code', None)))
                print(json.dumps(dict(transportError=str(error))), flush=True)
            stop.wait(min(2, max(0, deadline - time.monotonic())))
        report['finalUptime'] = previous
        capture_diagnostics(api, report, 'final')
        failures = sampling_failures(report)
        require(not failures, '; '.join(failures))
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
    finally:
        stop.set()
        if serial_thread: serial_thread.join(2)
        if stream: stream.close()
        log = (out / 'serial.log').read_text(encoding='utf8', errors='replace') if (out / 'serial.log').exists() else ''
        report['serialCriticalLines'] = [line for line in log.splitlines() if re.search(
            'Guru Meditation|Backtrace:|Brownout|stack overflow|Stack canary|rst:|watchdog|assert failed', line, re.I)]
        if report.get('serialError') or report['serialCriticalLines']: report['status'] = 'FAIL'
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
        print(json.dumps(dict(status=report['status'], report=str(out / 'report.json'),
                             error=report.get('error'), unverified=report['unverified'])), flush=True)
    return 0 if report['status'] == 'PASS' else 2


if __name__ == '__main__':
    raise SystemExit(main())
