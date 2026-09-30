"""Acceptance must reject missing data and restore outputs even after a failed command."""
import importlib.util
import contextlib
import io
import itertools
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('modbus_acceptance', ROOT / 'scripts/test-modbus-device.py')
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class ModbusDeviceAcceptanceTests(unittest.TestCase):
    def test_transport_recovery_keeps_evidence_but_does_not_pass(self):
        import requests
        client = MagicMock()
        client.__enter__.return_value = client
        client.post.return_value.json.return_value = dict(success=True)
        info_calls = 0
        def request(method, url, **kwargs):
            nonlocal info_calls
            if url.endswith('/info'):
                info_calls += 1
                if info_calls == 2:
                    raise requests.ConnectTimeout('test transport interruption')
                data = dict(network=dict(macAddress='14:C1:9F:C1:52:D8'),
                            device=dict(chipModel='ESP32-S3'), uptime=dict(seconds=100 + info_calls))
            elif url.endswith('/protocol/config'):
                data = dict(modbusRtu=dict(master=dict(devices=[dict(
                    enabled=True, deviceType='relay', slaveAddress=6, channelCount=4, controlProtocol=1)])))
            elif url.endswith('/coil/status'):
                data = dict(states=[False] * 4)
            elif url.endswith('/status'):
                data = dict(tasks=[dict(enabled=True, status='ok', cachedData=dict(values=[12]),
                                       mappings=[dict(sensorId='temperature')])])
            else:
                data = dict(state=1)
            response = MagicMock(status_code=200)
            response.json.return_value = dict(success=True, data=data)
            return response
        client.request.side_effect = request
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'acceptance'
            argv = ['test-modbus-device.py', '--url', 'http://test.invalid', '--expected-mac',
                    '14:C1:9F:C1:52:D8', '--seconds', '30', '--out', str(out)]
            with patch('sys.argv', argv), patch.dict(acceptance.os.environ, FASTBEE_PASSWORD='test'), \
                 patch.object(requests, 'Session', return_value=client), \
                 patch.object(acceptance.time, 'monotonic', side_effect=itertools.count()), \
                 patch.object(acceptance.threading.Event, 'wait', return_value=False), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main(), 2)
            report = json.loads((out / 'report.json').read_text(encoding='utf8'))
            self.assertEqual(report['status'], 'FAIL')
            self.assertEqual(len(report['transportErrors']), 1)
            self.assertGreater(len(report['samples']), 0)
            self.assertGreater(report['finalUptime'], report['initialUptime'])
            self.assertIn('Transport errors', report['error'])
            self.assertEqual(report['relay']['status'], 'PASS')
            self.assertTrue(any(row.get('error') == 'ConnectTimeout' and row['status'] is None
                                for row in report['requests']))

    def test_stopped_protocol_and_http_guard_keep_observing_recovery(self):
        import requests
        client = MagicMock()
        client.__enter__.return_value = client
        client.post.return_value.json.return_value = dict(success=True)
        status_calls = 0
        uptimes = itertools.count(100)

        def request(method, url, **kwargs):
            nonlocal status_calls
            response = MagicMock(status_code=200)
            if url.endswith('/info'):
                data = dict(network=dict(macAddress='14:C1:9F:C1:52:D8'),
                            device=dict(chipModel='ESP32-S3'), uptime=dict(seconds=next(uptimes)))
            elif url.endswith('/protocol/config'):
                data = dict(modbusRtu=dict(master=dict(devices=[dict(
                    enabled=True, deviceType='relay', slaveAddress=6, channelCount=4, controlProtocol=1)])))
            elif url.endswith('/coil/status'):
                data = dict(states=[False] * 4)
            elif url.endswith('/status'):
                status_calls += 1
                if status_calls == 2:
                    response.status_code = 503
                    response.raise_for_status.side_effect = requests.HTTPError('guard active', response=response)
                data = dict(status='stopped', tasks=[]) if status_calls == 1 else dict(
                    tasks=[dict(enabled=True, status='ok', cachedData=dict(values=[12]),
                                mappings=[dict(sensorId='temperature')])])
            else:
                data = dict(state=1)
            response.json.return_value = dict(success=True, data=data)
            return response

        client.request.side_effect = request
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'acceptance'
            argv = ['test-modbus-device.py', '--url', 'http://test.invalid', '--expected-mac',
                    '14:C1:9F:C1:52:D8', '--seconds', '80', '--out', str(out)]
            with patch('sys.argv', argv), patch.dict(acceptance.os.environ, FASTBEE_PASSWORD='test'), \
                 patch.object(requests, 'Session', return_value=client), \
                 patch.object(acceptance.time, 'monotonic', side_effect=itertools.count()), \
                 patch.object(acceptance.threading.Event, 'wait', return_value=False), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(acceptance.main(), 2)
            report = json.loads((out / 'report.json').read_text(encoding='utf8'))
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(len(report['samplingErrors']), 1)
        self.assertEqual(report['samples'][0]['mappingErrors'][0]['id'], 'modbus')
        self.assertFalse(report['samples'][-1]['mappingErrors'])
        self.assertGreater(report['finalUptime'], report['initialUptime'])
        self.assertEqual(report['transportErrors'][0]['status'], 503)
        self.assertTrue(any(row['status'] == 503 for row in report['requests']))

    def test_recovered_last_sample_does_not_erase_missing_data(self):
        report = dict(samples=[dict(mappingErrors=[dict(id='temp')]), dict(mappingErrors=[])])
        self.assertIn('Some mapped sensors', acceptance.sampling_failures(report)[0])
        self.assertEqual(acceptance.sampling_failures(dict(samples=[dict(mappingErrors=[])])), [])
        self.assertIn('No complete monitoring', acceptance.sampling_failures(dict(samples=[]))[0])

    def test_pm_rewire_recovery_keeps_prior_missing_channels_failed(self):
        task = dict(enabled=True, slaveAddress=4, status='error', lastError=1,
                    cachedData=dict(valid=False, values=[0, 0]),
                    mappings=[dict(sensorId='pm25', regOffset=0, dataType=0, scaleFactor=1),
                              dict(sensorId='pm10', regOffset=1, dataType=0, scaleFactor=1)])
        missing, errors = acceptance.mapped_readings(dict(tasks=[task]))
        self.assertEqual(missing, [])
        self.assertEqual({r['id'] for r in errors}, {'pm25', 'pm10'})
        task.update(status='ok', lastError=0, cachedData=dict(valid=True, values=[132, 139]))
        restored, recovered_errors = acceptance.mapped_readings(dict(tasks=[task]))
        self.assertEqual([(r['id'], r['value']) for r in restored], [('pm25', 132), ('pm10', 139)])
        self.assertEqual(recovered_errors, [])
        old_window = dict(samples=[dict(readings=missing, mappingErrors=errors),
                                   dict(readings=restored, mappingErrors=recovered_errors)])
        self.assertTrue(acceptance.sampling_failures(old_window))
        self.assertEqual(acceptance.sampling_failures(dict(samples=[old_window['samples'][-1]])), [])

    def test_unexpired_cache_cannot_hide_a_stalled_polling_scheduler(self):
        report = dict(samples=[dict(uptime=40, mappingErrors=[]), dict(uptime=80, mappingErrors=[])],
                      initialModbus=dict(totalPolls=10, tasks=[dict(enabled=True, pollInterval=30)]),
                      finalModbus=dict(totalPolls=10))
        self.assertIn('Background polling did not advance', acceptance.sampling_failures(report)[0])
        report['finalModbus']['totalPolls'] = 15
        self.assertEqual(acceptance.sampling_failures(report), [])

    def test_relay_timeout_is_reported_without_actuator_writes_or_aborting_monitor(self):
        for exercise in (False, True):
            with self.subTest(exercise=exercise):
                calls, report = [], {}
                def api(route, params=None, method='GET'):
                    calls.append((route, method))
                    if route.endswith('/config'):
                        return dict(modbusRtu=dict(master=dict(devices=[dict(
                            enabled=True, deviceType='relay', slaveAddress=6, channelCount=4, controlProtocol=1)])))
                    raise RuntimeError('504 relay timeout')
                args = SimpleNamespace(exercise_relay=exercise, relay_slave=6, channel=0, cycles=2)
                acceptance.probe_relay(api, args, report)
                self.assertEqual(report['status'], 'FAIL')
                self.assertEqual(report['parameters']['channelCount'], 4)
                self.assertEqual(report['parameters']['mode'], 'register')
                self.assertTrue(all(method == 'GET' for _, method in calls))
                failures = acceptance.sampling_failures(dict(samples=[dict(mappingErrors=[])], relay=report))
                self.assertIn('504 relay timeout', failures[0])

    def test_all_register_formats_and_legitimate_zero(self):
        cases = [([0], 0, 1, 0), ([65526], 1, .1, -1),
                 ([65535, 65535], 2, 1, 4294967295), ([65535, 65526], 3, 1, -10),
                 ([0x41C0, 0], 4, 1, 24), ([12], 0, 0, 0)]
        for words, kind, scale, expected in cases:
            with self.subTest(kind=kind, words=words):
                self.assertEqual(acceptance.decode_mapping(
                    dict(status='ok', cachedData=dict(values=words)),
                    dict(dataType=kind, scaleFactor=scale)), expected)

    def test_failed_stale_and_partial_samples_are_not_zero(self):
        for task, mapping in [
            (dict(status='stale', cachedData=dict(values=[123])), {}),
            (dict(status='ok', cachedData=dict(valid=False, values=[123])), {}),
            (dict(status='ok', lastError=1, cachedData=dict(values=[123])), {}),
            (dict(status='ok', cachedData=dict(values=[True])), {}),
            (dict(status='ok', cachedData=dict(values=[1])), dict(dataType=4)),
            (dict(status='ok', cachedData=dict(values=[1])), dict(scaleFactor=None)),
            (dict(status='ok', cachedData=dict(values=[0x7f80, 0])), dict(dataType=4)),
        ]:
            with self.subTest(task=task, mapping=mapping), self.assertRaises(ValueError):
                acceptance.decode_mapping(task, mapping)
        with self.assertRaises(ValueError):
            acceptance.mapped_readings(dict(tasks=[]))

    def test_unknown_relay_state_never_passes(self):
        for states in [[], [False], [False, None], [False, 0], [False, 'false']]:
            with self.subTest(states=states), self.assertRaises(ValueError):
                acceptance.validate_states(dict(states=states), 2)
        self.assertEqual(acceptance.validate_states(dict(states=[False, True]), 2), [False, True])

    def test_reboot_is_a_failure(self):
        self.assertEqual(acceptance.verify_uptime(100, 101), 101)
        for uptime in [1, None, True, float('nan')]:
            with self.subTest(uptime=uptime), self.assertRaises(ValueError):
                acceptance.verify_uptime(100, uptime)

    def test_relay_restores_original_channel_after_success_and_failure(self):
        for failure in [False, True]:
            with self.subTest(failure=failure):
                states, writes, report = [True, False], [], {}
                def api(route, params=None, method='GET'):
                    if route.endswith('/info'):
                        return dict(uptime=dict(seconds=100))
                    if method == 'POST':
                        writes.append(dict(params))
                        states[params['channel']] = params['action'] == 'on'
                        if failure and len(writes) == 1:
                            raise RuntimeError('Simulated disconnect after command')
                        return {}
                    return dict(states=states.copy())
                args = (api, dict(slaveAddress=6, coilBase=0, channelCount=2, mode='register'), 0, 2, report)
                with patch.object(acceptance.time, 'sleep'):
                    if failure:
                        with self.assertRaises(RuntimeError):
                            acceptance.exercise_relay(*args)
                    else:
                        acceptance.exercise_relay(*args)
                self.assertEqual(states, [True, False])
                self.assertTrue(report['restored'])
                self.assertEqual(len(writes), 2, 'Do not reapply an already restored output')
                self.assertEqual(writes[-1]['action'], 'on')
                self.assertTrue(all(w['channel'] == 0 and w['mode'] == 'register' for w in writes))


if __name__ == '__main__':
    unittest.main()
