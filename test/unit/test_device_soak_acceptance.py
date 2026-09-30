import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock
import requests
import copy
import contextlib
import io
import json
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('device_soak', Path(__file__).resolve().parents[2]/'scripts/test-device-soak.py')
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)

class SoakAcceptanceTests(unittest.TestCase):
    def report(self, seconds=300, interval=30):
        return dict(requestedSeconds=seconds, intervalSeconds=interval, samples=0, attempts=0,
                    samplingGaps=0, minimumRequiredSamples=soak.minimum_samples(seconds, interval),
                    stabilityStatus='RUNNING', measurementStatus='RUNNING')

    def test_attempts_do_not_hide_successful_sample_gaps(self):
        report = self.report()
        coverage = soak.CoverageTracker(report, 100, 30)
        for tick in range(100, 281, 30):
            coverage.attempt(tick)
            coverage.complete(tick, str(tick), tick == 100)
        self.assertEqual(report['attempts'], 7)
        self.assertEqual(report['samples'], 1)
        self.assertEqual(report['maximumAttemptGapSeconds'], 30)
        self.assertEqual(report['maximumSamplingGapSeconds'], 180)
        self.assertEqual(report['samplingGaps'], 1)
        self.assertEqual(report['measurementUnavailableSamples'], 6)
        self.assertFalse(report['coveragePassed'])

    def test_recovery_retains_failed_gap_and_counts_distinct_episodes(self):
        report = self.report(seconds=900)
        coverage = soak.CoverageTracker(report, 0, 30)
        coverage.complete(0, 'start', True)
        coverage.update(120); coverage.update(150)
        coverage.complete(180, 'recovered', True)
        self.assertEqual(report['samplingGaps'], 1)
        self.assertEqual(report['lastSuccessfulSampleAgeSeconds'], 0)
        coverage.update(300)
        self.assertEqual(report['samplingGaps'], 2)
        self.assertEqual(report['stabilityStatus'], 'FAIL')

    def test_no_samples_and_trailing_hang_fail_coverage(self):
        report = self.report()
        coverage = soak.CoverageTracker(report, 10, 30)
        coverage.update(131)
        self.assertEqual(report['samplingGaps'], 1)
        self.assertIsNone(report['lastSuccessfulSampleAgeSeconds'])
        self.assertFalse(report['coveragePassed'])
        report = self.report()
        coverage = soak.CoverageTracker(report, 0, 30)
        for tick in range(0, 181, 30): coverage.complete(tick, str(tick), True)
        coverage.update(300)
        self.assertEqual(report['samplingGaps'], 1)
        self.assertTrue(report['durationComplete'])
        self.assertFalse(report['coveragePassed'])

    def test_real_eight_hours_required_without_rounding_or_reused_uptime(self):
        report = self.report(seconds=28800)
        coverage = soak.CoverageTracker(report, 50000, 30)
        for tick in range(0, 28800, 30): coverage.complete(50000 + tick, str(tick), True)
        coverage.update(50000 + 28799.999)
        self.assertFalse(report['durationComplete'])
        self.assertFalse(report['coveragePassed'])
        coverage.complete(78800, 'end', True)
        report['pollsAdvanced'] = True
        soak.finish_report(report, completed=True)
        self.assertEqual(report['status'], 'PASS')

    def test_fatal_or_interrupt_cannot_erase_previous_failures(self):
        for interrupted in (False, True):
            report = self.report()
            report.update(stabilityStatus='FAIL', measurementStatus='FAIL', transportErrors=1, invalidSoilSamples=1)
            soak.finish_report(report, interrupted=interrupted)
            self.assertEqual(report['stabilityStatus'], 'FAIL')
            self.assertEqual(report['measurementStatus'], 'FAIL')
            self.assertEqual(report['status'], 'INTERRUPTED' if interrupted else 'FAIL')
        report = self.report()
        soak.finish_report(report, interrupted=True)
        self.assertEqual(report['stabilityStatus'], 'INCOMPLETE')
        self.assertEqual(report['measurementStatus'], 'INCOMPLETE')

    def test_invalid_humidity_does_not_become_stability_or_measurement_pass(self):
        report = self.report(seconds=120)
        coverage = soak.CoverageTracker(report, 0, 30)
        for tick in range(0, 121, 30): coverage.complete(tick, str(tick), True)
        report.update(invalidSoilSamples=5, measurementStatus='FAIL', pollsAdvanced=True)
        soak.finish_report(report, completed=True)
        self.assertEqual(report['stabilityStatus'], 'PASS')
        self.assertEqual(report['measurementStatus'], 'FAIL')
        self.assertEqual(report['status'], 'FAIL')

    def test_latest_error_survives_example_cap_and_keeps_error_types(self):
        report = self.report()
        for n in range(40): soak.record_error(report, 'sampling', ValueError('old'), str(n))
        event = soak.record_error(report, 'sampling', OSError('[WinError 10051] network unreachable'), 'latest')
        self.assertEqual(len(report['errorExamples']), 30)
        self.assertEqual(event['errorType'], 'NETWORK_UNREACHABLE')
        self.assertEqual(report['latestError'], event)
        self.assertEqual(report['lastErrorByKind']['sampling'], event)
        self.assertEqual(report['errorTypeCounts']['ValueError'], 40)
        self.assertEqual(soak.classify_error(OSError('[WinError 10065]')), 'HOST_UNREACHABLE')
        self.assertEqual(soak.classify_error(requests.ConnectTimeout('timeout')), 'CONNECT_TIMEOUT')
        self.assertEqual(soak.classify_error(requests.ReadTimeout('timeout')), 'READ_TIMEOUT')

    def test_invalid_api_json_is_rejected_instead_of_fatal_attribute_error(self):
        for body in (None, [], 'bad', {'success': False}):
            session = Mock()
            session.get.return_value = Mock(status_code=200, json=lambda: body)
            with self.assertRaises(ValueError): soak.read_api(session, 'http://device', '/api/info', None, {})

    def test_onboard_config_rejects_wrong_gpio_polarity_and_startup_state(self):
        valid = {'id':'do1','type':12,'enabled':True,'pins':[15], 'params':{'initialState':1,'invertLogic':False}}
        soak.validate_onboard_config(valid, 'do1', 15)
        for field, value in [('pins',[16]), ('id','modbus:0'), ('enabled',False)]:
            invalid = copy.deepcopy(valid); invalid[field] = value
            with self.assertRaises(ValueError): soak.validate_onboard_config(invalid,'do1',15)
        for field, value in [('initialState',0), ('initialState',True), ('invertLogic',True)]:
            invalid = copy.deepcopy(valid); invalid['params'][field] = value
            with self.assertRaises(ValueError): soak.validate_onboard_config(invalid,'do1',15)

    def test_rule_trigger_and_counters_must_stay_valid(self):
        rule = {'id':'photo','enabled':True,'actions':[{'targetPeriphId':'do1','actionType':30}],
                'triggers':[{'triggerType':4,'triggerPeriphId':'di1','eventId':'ds:di1','compareValue':'0','triggerCount':0,'lastTriggerTime':0}]}
        for field, value in [('eventId','system_ready'), ('triggerPeriphId','di2'), ('compareValue','1'),
                             ('triggerCount',-1), ('triggerCount',True), ('lastTriggerTime',None)]:
            invalid = copy.deepcopy(rule); invalid['triggers'][0][field] = value
            with self.assertRaises(ValueError): soak.onboard_relay_reading({'state':0},invalid,'do1','photo')

    def _run_main_scenario(self, mode='401'):
        root = Path(__file__).resolve().parents[2]
        configs = {x['id']: x for x in json.loads((root/'docs/ecm50a-s3-f16r8/peripherals.json').read_text(encoding='utf8'))['peripherals']}
        protocol = json.loads((root/'docs/ecm50a-s3-f16r8/protocol.json').read_text(encoding='utf8'))
        clock = [0.0]
        session = Mock()
        def response(data, status=200):
            result = requests.Response(); result.status_code = status
            result._content = json.dumps({'success': True, 'data': data}).encode()
            return result
        session.post.return_value = response({})
        def get(url, params=None, **kwargs):
            if url.endswith('/api/system/info'):
                if mode == 'interrupt' and clock[0] >= 60: raise KeyboardInterrupt()
                if mode in ('401','interrupt') and clock[0] >= 30: return response({}, 401)
                mac = '14:C1:9F:C1:52:D9' if mode == 'identity' and clock[0] >= 30 else '14:C1:9F:C1:52:D8'
                return response({'network': {'macAddress': mac}, 'uptime': {'seconds': 332+clock[0]}, 'memory': {}})
            if url.endswith('/api/peripherals/'): return response(configs[params['id']])
            if url.endswith('/api/protocol/config'): return response(protocol)
            if url.endswith('/api/modbus/status'): return response({'totalPolls': 1+int(clock[0]), 'tasks': [{'enabled': True, 'pollInterval': 30}]})
            if url.endswith('/api/peripherals/read'):
                ident = params['id']
                if ident in ['di1','di2']: return response({'state': 0})
                if ident == 'do1': return response({'state': '1' if clock[0] >= 30 else 1})
                return response({'id': ident, 'calibratedVoltage': True, 'pinVoltageMv': 590 if 'humidity' in ident else 1800,
                    'quality': 'low' if 'humidity' in ident else 'valid', 'valueValid': 'humidity' not in ident, 'value': None if 'humidity' in ident else 20})
            if url.endswith('/api/modbus/coil/status'): return response({'states': [True] * 4})
            if url.endswith('/api/periph-exec'): return response({'id':'photo','enabled':True,
                'actions':[{'targetPeriphId':'do1','actionType':30}], 'triggers':[{'triggerType':4,
                'triggerPeriphId':'di1','eventId':'ds:di1','compareValue':'0','triggerCount':0,'lastTriggerTime':0}]})
            raise AssertionError('Unexpected request: '+url)
        session.get.side_effect = get
        channels = [{'id': m['sensorId']} for t in protocol['modbusRtu']['master']['tasks'] if t.get('enabled') for m in t.get('mappings',[])]
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)/'run'
            argv = ['soak', '--url', 'http://offline.invalid', '--expected-mac', '14:C1:9F:C1:52:D8', '--seconds','120','--interval','30','--out',str(out)]
            if mode == 'onboard': argv += ['--onboard-relay-id','do1','--onboard-rule-id','photo']
            with patch('sys.argv', argv), patch.dict('os.environ', {'FASTBEE_PASSWORD':'offline-test'}), \
                 patch('requests.Session', return_value=session), patch.object(soak.time,'monotonic',side_effect=lambda:clock[0]), \
                 patch.object(soak.time,'sleep',side_effect=lambda n:clock.__setitem__(0,clock[0]+n)), \
                 patch.object(soak.modbus,'mapped_readings',return_value=(channels,[])), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = soak.main()
            report = json.loads((out/'report.json').read_text(encoding='utf8'))
            rows = [json.loads(x) for x in (out/'samples.jsonl').read_text(encoding='utf8').splitlines()]
            events = [json.loads(x) for x in (out/'events.jsonl').read_text(encoding='utf8').splitlines()]
            session.post.assert_called_once(); session.close.assert_called_once()
            return code, report, rows, events

    def test_main_writes_failed_rows_without_relogin_and_detects_coverage_loss(self):
        code, report, rows, events = self._run_main_scenario()
        self.assertEqual(code, 2)
        self.assertEqual(report['samples'], 1)
        self.assertEqual(report['attempts'], len(rows))
        self.assertEqual(report['samplingGaps'], 1)
        self.assertEqual(report['httpStatusCounts'], {'401': 4})
        self.assertEqual(report['stabilityStatus'], 'FAIL')
        self.assertEqual(report['measurementStatus'], 'FAIL')
        self.assertFalse(report['coveragePassed'])
        self.assertTrue(report['durationComplete'])
        self.assertTrue(all(not r['complete'] for r in rows[1:]))
        self.assertEqual(events[-1]['errorType'], 'HTTP_401')

    def test_main_stops_identity_change_after_recording_failed_attempt(self):
        _, report, rows, _ = self._run_main_scenario('identity')
        self.assertEqual(len(rows), 2)
        self.assertIn('identity changed', rows[-1]['error'])
        self.assertFalse(rows[-1]['complete'])
        self.assertFalse(report['durationComplete'])
        self.assertEqual(report['stabilityStatus'], 'FAIL')

    def test_main_interrupt_preserves_prior_http_and_measurement_failure(self):
        _, report, rows, _ = self._run_main_scenario('interrupt')
        self.assertEqual(report['status'], 'INTERRUPTED')
        self.assertEqual(report['stabilityStatus'], 'FAIL')
        self.assertEqual(report['measurementStatus'], 'FAIL')
        self.assertFalse(rows[-1]['complete'])
        self.assertEqual(report['httpStatusCounts'], {'401':1})

    def test_main_invalid_onboard_readbacks_are_not_successful_samples(self):
        _, report, rows, _ = self._run_main_scenario('onboard')
        self.assertEqual(report['onboardRelayReadErrors'], 4)
        self.assertEqual(report['samples'], 1)
        self.assertFalse(report['coveragePassed'])
        self.assertTrue(all(not r['complete'] for r in rows[1:]))

    def test_onboard_readback_accepts_both_toggle_states_without_claiming_lamp_feedback(self):
        rule = dict(id='photo', enabled=True, actions=[dict(targetPeriphId='do1', actionType=30)],
                    triggers=[dict(triggerType=4, triggerPeriphId='di1', eventId='ds:di1',
                                   compareValue='0', triggerCount=2, lastTriggerTime=11000)])
        for state in (0, 1):
            result = soak.onboard_relay_reading({'state': state}, rule, 'do1', 'photo')
            self.assertEqual(result['state'], state)
            self.assertEqual(result['triggerCount'], 2)
            self.assertIn('not observed', result['evidence'])
        for invalid in (None, True, '1', 2):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                soak.onboard_relay_reading({'state': invalid}, rule, 'do1', 'photo')
        rule['enabled'] = False
        with self.assertRaises(ValueError):
            soak.onboard_relay_reading({'state': 1}, rule, 'do1', 'photo')
        rule['enabled'] = True
        rule['actions'][0]['targetPeriphId'] = 'modbus:0'
        with self.assertRaises(ValueError):
            soak.onboard_relay_reading({'state': 1}, rule, 'do1', 'photo')

    def test_http_failures_are_recorded_without_retry_or_login(self):
        for status in (401, 429, 503):
            with self.subTest(status=status):
                report = dict(httpErrorCount=0, httpStatusCounts={}, http429Count=0)
                session = Mock()
                response = requests.Response()
                response.status_code = status
                session.get.return_value = response
                with self.assertRaises(requests.HTTPError):
                    soak.read_api(session, 'http://device', '/api/system/info', None, report)
                session.get.assert_called_once()
                session.post.assert_not_called()
                self.assertEqual(report['httpErrorCount'], 1)
                self.assertEqual(report['httpStatusCounts'], {str(status): 1})
                self.assertEqual(report['http429Count'], int(status == 429))
                session.get.return_value = Mock(status_code=200, json=lambda: {'success': True, 'data': {'uptime': 1}})
                self.assertEqual(soak.read_api(session, 'http://device', '/api/system/info', None, report), {'uptime': 1})
                self.assertEqual(report['httpErrorCount'], 1)

    def test_sparse_samples_cannot_stand_in_for_eight_hours(self):
        self.assertEqual(soak.minimum_samples(28800,30),864)
        self.assertEqual(soak.minimum_samples(30,10),3)
        self.assertEqual(soak.minimum_samples(30,30),2)

    def test_polling_reset_is_detected_even_when_device_uptime_increases(self):
        self.assertEqual(soak.poll_health(100, 3, 30, 30), (True, False))

    def test_polling_stall_exceeds_configured_interval_and_grace(self):
        self.assertEqual(soak.poll_health(100, 100, 60, 30), (False, True))
        self.assertEqual(soak.poll_health(100, 100, 30, 30), (False, False))

    def test_new_polling_and_progress_are_not_stalls(self):
        self.assertEqual(soak.poll_health(None, 0, 0, 30), (False, False))
        self.assertEqual(soak.poll_health(100, 105, 60, 30), (False, False))

    def test_missing_or_non_numeric_poll_counter_cannot_pass(self):
        for value in (None, True, -1, '100'):
            with self.assertRaises(ValueError):
                soak.poll_health(100, value, 30, 30)

if __name__ == '__main__':
    unittest.main()
