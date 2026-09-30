"""Apply and verify COM15 onboard DO1 NC-lamp startup configuration.

This deliberately operates the lamp and restarts the controller only with
--apply. GPIO readback is firmware state, not physical contact/lamp feedback.
Credentials use FASTBEE_USERNAME / FASTBEE_PASSWORD and are never recorded.
"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time

import requests

RULE_ID = 's3_photoelectric_relay_toggle'


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                    separators=(',', ':')).encode()).hexdigest()


def config_only(value):
    return {key: value[key] for key in ('id', 'name', 'type', 'enabled', 'pins', 'params')}


def rule_config(value):
    ignored = {'lastTriggerTime', 'triggerCount', 'triggerPeriphName',
               'targetPeriphName', 'targetPeriphType'}
    if isinstance(value, dict):
        return {k: rule_config(v) for k, v in value.items() if k not in ignored}
    if isinstance(value, list):
        return [rule_config(v) for v in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--expected-mac', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    report = dict(status='RUNNING',startedAt=datetime.now(timezone.utc).isoformat(),
                  url=args.url,expectedMac=args.expected_mac,scope='onboard do1 GPIO15',
                  restartCycles=[],actionTests=[],physicalLamp='NOT_OBSERVED',
                  physicalPhotoelectricEdge='NOT_EXERCISED',coldPowerCycle='NOT_PERFORMED',
                  readbackKind='firmware runtime GPIO state; no contact feedback')
    session = requests.Session()
    session.trust_env = False
    applied = False

    def save():
        report['updatedAt'] = datetime.now(timezone.utc).isoformat()
        pending = out / 'report.pending'
        pending.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
        pending.replace(out / 'report.json')

    def request(path, method='GET', **kwargs):
        time.sleep(.25)
        response = session.request(method, args.url.rstrip('/') + path, timeout=(3, 15), **kwargs)
        response.raise_for_status()
        body = response.json()
        if body.get('success') is not True:
            raise RuntimeError('API rejected ' + path + ': ' + str(body.get('message')))
        return body

    def api(path, **kwargs):
        return request(path, **kwargs).get('data')

    def login():
        request('/api/auth/login', 'POST', data={
            'username': os.environ.get('FASTBEE_USERNAME', 'admin'),
            'password': os.environ['FASTBEE_PASSWORD']})

    def identity():
        info = api('/api/system/info')
        actual = info['network']['macAddress'].replace(':', '').lower()
        assert actual == args.expected_mac.replace(':', '').lower(), 'Wrong device MAC'
        return info

    def config(ident):
        return api('/api/peripherals/', params={'id': ident})

    def state(ident='do1'):
        value = api('/api/peripherals/read', params={'id': ident})['state']
        assert type(value) is int and value in (0, 1), 'Invalid GPIO state'
        return value

    def rule():
        return api('/api/periph-exec', params={'id': RULE_ID})

    def set_state(value):
        request('/api/peripherals/write', 'POST', data={'id': 'do1', 'state': value})
        assert state() == value, 'GPIO write/readback mismatch'

    save()
    try:
        login()
        report['beforeUptime'] = identity()['uptime']['seconds']
        items = api('/api/peripherals')
        if isinstance(items, dict):
            items = items['peripherals']
        before = {p['id']: config_only(config(p['id'])) for p in items}
        do1 = before['do1']
        assert do1['type'] == 12 and do1['pins'] == [15] and do1['enabled']
        assert do1['params'].get('ioSource', 0) == 0 and not do1['params'].get('invertLogic', False)
        assert before['do2']['pins'] == [16] and before['do2']['params']['initialState'] == 0
        rules = {r['id']: rule_config(api('/api/periph-exec', params={'id': r['id']}))
                 for r in api('/api/periph-exec')}
        photo = rules[RULE_ID]
        assert photo['enabled'] and len(photo['actions']) == 1
        assert photo['actions'][0]['targetPeriphId'] == 'do1' and photo['actions'][0]['actionType'] == 30
        assert photo['triggers'][0]['eventId'] == 'ds:di1' and photo['triggers'][0]['compareValue'] == '0'
        protocol_hash = digest(api('/api/protocol/config'))
        report['before'] = dict(peripherals=before, rules=rules, protocolSha256=protocol_hash)
        expected = copy.deepcopy(before)
        expected['do1']['params']['initialState'] = 1
        save()
        if args.apply:
            if do1['params']['initialState'] != 1:
                report['updateResponse'] = request('/api/peripherals/update', 'POST',
                                                   data={'id': 'do1', 'initialState': 1})
            applied = True
            report['savedDo1'] = config('do1')
            assert config_only(report['savedDo1']) == expected['do1'], 'Unexpected saved fields'
            report['afterApplyState'] = state()
            assert report['afterApplyState'] == 1, 'Configured startup value not applied'
            save()
            for cycle in (1, 2):
                set_state(0)  # Challenge restart from the opposite state.
                entry = dict(cycle=cycle, beforeState=state(), beforeUptime=identity()['uptime']['seconds'])
                report['restartCycles'].append(entry)
                save()
                request('/api/system/restart', 'POST')
                entry['restartRequested'] = True
                save()
                time.sleep(4)
                start = time.monotonic()
                relogged = False
                while time.monotonic() - start < 90:
                    try:
                        info = identity()
                        if info['uptime']['seconds'] < entry['beforeUptime']:
                            entry['firstReachableUptime'] = info['uptime']['seconds']
                            break
                    except requests.HTTPError as error:
                        if error.response.status_code == 401 and not relogged:
                            login()
                            relogged = True
                        else:
                            raise
                    except (requests.ConnectionError, requests.Timeout):
                        pass
                    time.sleep(2)
                else:
                    raise RuntimeError('Restart not confirmed within 90 seconds')
                entry['observations'] = []
                for index in range(3):
                    if index:
                        time.sleep(5)
                    info = identity()
                    trigger = rule()['triggers'][0]
                    observation = dict(uptime=info['uptime']['seconds'], do1=state(), do2=state('do2'),
                                       triggerCount=trigger['triggerCount'], lastTriggerTime=trigger['lastTriggerTime'])
                    entry['observations'].append(observation)
                    save()
                    assert observation['do1'] == 1 and observation['do2'] == 0, 'Unexpected boot GPIO state'
                    assert observation['triggerCount'] == 0, 'Photoelectric event occurred during boot check'
                assert config_only(config('do1')) == expected['do1'], 'Initial state not persisted'
                entry['status'] = 'PASS'
                save()
            for expected_state in (0, 1):
                result = request('/api/periph-exec/run', 'POST', data={'id': RULE_ID})
                terminal = None
                for attempt in range(15):
                    time.sleep(1)
                    results = api('/api/periph-exec/results',
                                  params={'operationId': result['operationId'], 'includeActions': 'true'})
                    terminal = next((r for r in results if r['ruleId'] == RULE_ID and r.get('terminal')), None)
                    if terminal:
                        break
                entry = dict(expectedState=expected_state, execution=terminal, readback=state())
                report['actionTests'].append(entry)
                save()
                assert terminal and terminal['state'] == 'success' and terminal.get('actions')
                assert all(action['success'] for action in terminal['actions'])
                assert entry['readback'] == expected_state, 'Photoelectric action path did not toggle'
                assert state('do2') == 0, 'Relay two unexpectedly changed'
            after = {ident: config_only(config(ident)) for ident in before}
            assert after == expected, 'Unrelated peripheral configuration changed'
            assert {r['id'] for r in api('/api/periph-exec')} == set(rules), 'Rule set changed'
            for ident, old in rules.items():
                assert rule_config(api('/api/periph-exec', params={'id': ident})) == old, 'Rule changed: ' + ident
            assert digest(api('/api/protocol/config')) == protocol_hash, 'Protocol configuration changed'
            report.update(otherPeripheralsPreserved=True, rulesPreserved=True, protocolPreserved=True,
                          finalState=state(), initialStatePersisted=True, savedConfigurationSha256=digest(after))
            assert report['finalState'] == 1
            report['status'] = 'PASS'
        else:
            report['status'] = 'INSPECTED'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
    finally:
        if applied and report['status'] != 'PASS':
            try:
                set_state(1)
                report['failureFinalState'] = 1
            except Exception as error:
                report['restoreError'] = str(error)
        try:
            request('/api/auth/logout', 'POST')
        except Exception:
            pass
        session.close()
        save()
        print(json.dumps({k: v for k, v in report.items() if k != 'before'}, ensure_ascii=False), flush=True)
    return 0 if report['status'] in ('PASS', 'INSPECTED') else 1


if __name__ == '__main__':
    raise SystemExit(main())
