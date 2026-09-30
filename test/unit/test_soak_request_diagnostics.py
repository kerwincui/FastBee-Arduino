"""Redacted request evidence must not change sampling, retry, or failure rules."""
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import Mock
import requests

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('diagnostic_soak', ROOT / 'scripts/test-device-soak.py')
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class SoakRequestDiagnosticTests(unittest.TestCase):
    def test_connect_timeout_records_attempted_cookie_without_retries(self):
        request = requests.Request('GET', 'http://127.0.0.1/api/system/info',
                                   headers={'Cookie': 'sessionId=SECRET', 'Authorization': 'Bearer SECRET'}).prepare()
        session = Mock()
        session.get.side_effect = requests.ConnectTimeout('connect timed out', request=request)
        report = {}
        with self.assertRaises(requests.ConnectTimeout):
            soak.read_api(session, 'http://127.0.0.1', '/api/system/info', None, report)
        session.get.assert_called_once_with('http://127.0.0.1/api/system/info', params=None, timeout=(3, 12))
        session.post.assert_not_called()
        diagnostic = report['lastRequestDiagnostic']
        self.assertEqual(diagnostic['failurePhase'], 'connect')
        self.assertTrue(diagnostic['cookieHeaderPresent'])
        self.assertTrue(diagnostic['authorizationHeaderPresent'])
        self.assertIsNone(diagnostic['httpStatus'])
        self.assertNotIn('SECRET', json.dumps(diagnostic))

    def test_401_preserves_failure_and_identifies_absent_cookie(self):
        response = requests.Response()
        response.status_code = 401
        response.request = requests.Request('GET', 'http://127.0.0.1/api/system/info').prepare()
        session = Mock()
        session.get.return_value = response
        report = dict(httpErrorCount=0, httpStatusCounts={}, http429Count=0)
        with self.assertRaises(requests.HTTPError):
            soak.read_api(session, 'http://127.0.0.1', '/api/system/info', None, report)
        self.assertEqual(report['httpStatusCounts'], {'401': 1})
        self.assertEqual(report['lastRequestDiagnostic']['failurePhase'], 'http_response')
        self.assertFalse(report['lastRequestDiagnostic']['cookieHeaderPresent'])
        session.get.assert_called_once()
        session.post.assert_not_called()

    def test_renewal_metadata_never_records_token_values_or_query(self):
        response = requests.Response()
        response.status_code = 200
        response._content = b'{"success":true,"data":{"uptime":100}}'
        response.request = requests.Request('GET', 'http://127.0.0.1/api/info', headers={'Cookie': 'sessionId=SECRET'}).prepare()
        response.headers['Set-Cookie'] = 'sessionId=SECRET; Max-Age=3600; HttpOnly'
        session = Mock()
        session.get.return_value = response
        report = {}
        self.assertEqual(soak.read_api(session, 'http://127.0.0.1', '/api/info?token=SECRET', None, report), {'uptime': 100})
        diagnostic = report['lastRequestDiagnostic']
        self.assertTrue(diagnostic['setCookieReceived'])
        self.assertIsNone(diagnostic['failurePhase'])
        self.assertNotIn('SECRET', json.dumps(diagnostic))
        self.assertEqual(diagnostic['path'], '/api/info')

    def test_missing_headers_are_unknown_and_read_timeout_is_distinct(self):
        diagnostic = soak.request_diagnostic('/api/info', soak.time.monotonic(), error=requests.ReadTimeout('timeout'))
        self.assertEqual(diagnostic['failurePhase'], 'response_read')
        self.assertIsNone(diagnostic['cookieHeaderPresent'])

    def test_truncated_response_is_body_failure_without_inventing_headers(self):
        error = requests.exceptions.ChunkedEncodingError(
            'IncompleteRead(967 bytes read, 551 more expected)')
        session = Mock()
        session.get.side_effect = error
        report = {}
        with self.assertRaises(requests.exceptions.ChunkedEncodingError):
            soak.read_api(session, 'http://offline.invalid', '/api/system/info', None, report)
        diagnostic = report['lastRequestDiagnostic']
        self.assertEqual(diagnostic['failurePhase'], 'response_body')
        self.assertEqual(diagnostic['errorType'], 'ChunkedEncodingError')
        self.assertIsNone(diagnostic['httpStatus'])
        self.assertIsNone(diagnostic['cookieHeaderPresent'])
        session.get.assert_called_once()
        session.post.assert_not_called()

    def test_body_errors_after_200_keep_status_and_redact_credentials(self):
        for error_class in (requests.exceptions.ChunkedEncodingError,
                            requests.exceptions.ContentDecodingError):
            with self.subTest(error_class=error_class.__name__):
                response = requests.Response()
                response.status_code = 200
                response.request = requests.Request('GET', 'http://offline.invalid/api/info',
                    headers={'Cookie': 'sessionId=SECRET'}).prepare()
                response.headers['Set-Cookie'] = 'sessionId=SECRET'
                error = error_class('SECRET response body', response=response)
                diagnostic = soak.request_diagnostic('/api/info?token=SECRET',
                    soak.time.monotonic(), response=response, error=error)
                self.assertEqual(diagnostic['failurePhase'], 'response_body')
                self.assertEqual(diagnostic['errorType'], error_class.__name__)
                self.assertEqual(diagnostic['httpStatus'], 200)
                self.assertTrue(diagnostic['cookieHeaderPresent'])
                self.assertNotIn('SECRET', json.dumps(diagnostic))

    def test_recovery_does_not_erase_body_error_history(self):
        report = {'stabilityStatus': 'FAIL', 'transportErrors': 1}
        error = requests.exceptions.ChunkedEncodingError('IncompleteRead')
        soak.record_error(report, 'sampling', error, '2026-09-30T10:20:51Z')
        session = Mock()
        session.get.return_value = Mock(status_code=200,
            json=lambda: {'success': True, 'data': {'uptime': 2}})
        self.assertEqual(soak.read_api(session, 'http://offline.invalid', '/api/info', None, report), {'uptime': 2})
        self.assertEqual(report['stabilityStatus'], 'FAIL')
        self.assertEqual(report['transportErrors'], 1)
        self.assertEqual(report['errorTypeCounts']['ChunkedEncodingError'], 1)
        self.assertEqual(report['lastErrorByKind']['sampling']['errorType'], 'ChunkedEncodingError')


if __name__ == '__main__':
    unittest.main()
