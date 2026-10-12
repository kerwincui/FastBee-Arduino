"""Compile real auth methods with a virtual clock; no device or login calls."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from email.message import Message
from http.cookiejar import CookieJar
from types import SimpleNamespace
from unittest.mock import patch
from urllib.request import Request
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class AuthCookieLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/security/AuthManager.cpp').read_text(encoding='utf8')
        context = (ROOT / 'src/network/WebHandlerContext.cpp').read_text(encoding='utf8')
        methods = '\n'.join(definition(source, signature) for signature in (
            'SessionStatus AuthManager::validateSession(',
            'void AuthManager::cleanupExpiredSessions(',
            'AuthResult AuthManager::verifySession(',
            'bool AuthManager::updateSessionAccessTime(',
            'static String readSessionCookieValue(',
            'String AuthManager::extractSessionIdFromRequest(',
            'String AuthManager::extractSessionIdFromCookie(',
        ))
        renew = definition(context, 'void WebHandlerContext::renewSessionCookie(')
        renew += '\n' + definition(context, 'AuthResult WebHandlerContext::authenticateRequest(')
        login = (ROOT / 'src/network/handlers/AuthRouteHandler.cpp').read_text(encoding='utf8')
        cookie_start = login.index('            if (securityConfig.cookieName.isEmpty())')
        cookie_end = login.index('            response->addHeader("Connection"', cookie_start)
        cookie = login[cookie_start:cookie_end]
        harness = (ROOT / 'test/unit/cpp/auth_cookie_lifecycle_harness.cpp').read_text(encoding='utf8')
        header = (ROOT / 'include/security/AuthManager.h').read_text(encoding='utf8')
        harness = harness.replace('PRODUCTION_SECURITY_CONFIG', definition(header, 'struct SecurityConfig') + ';')
        harness = harness.replace('PRODUCTION_METHODS', methods).replace('PRODUCTION_RENEW', renew)
        harness = harness.replace('PRODUCTION_LOGIN_COOKIE', cookie)
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise AssertionError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-auth-cookie-')
        cls.addClassCleanup(cls.directory.cleanup)
        temp = Path(cls.directory.name)
        cpp = temp / 'auth.cpp'
        cpp.write_text(harness, encoding='utf8')
        cls.exe = temp / ('auth.exe' if os.name == 'nt' else 'auth')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}', str(cpp), str(ROOT / 'src/network/LiteralStringMatch.cpp'), '-o', str(cls.exe)],
                               capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_case(self, case):
        ran = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_active_cookie_30_second_polls_for_nine_virtual_hours(self):
        self.run_case('active-30')

    def test_active_cookie_60_second_polls_for_nine_virtual_hours(self):
        self.run_case('active-60')

    def test_idle_expiry_cannot_be_resurrected(self):
        self.run_case('idle')

    def test_output_actor_is_session_scoped_and_never_reused(self):
        self.run_case('output-actor')

    def test_revocation_errors_and_bearer_only_do_not_issue_cookies(self):
        self.run_case('policy')

    def test_custom_cookie_exact_names_and_authorization_precedence(self):
        self.run_case('parsing')

    def test_active_session_and_cleanup_survive_millis_wrap(self):
        self.run_case('wrap')

    def test_cookie_renewal_runs_after_handler_in_admitted_middleware(self):
        server = (ROOT / 'src/network/WebConfigManager.cpp').read_text(encoding='utf8')
        start = server.index('if (checkIpRateLimit(request, retryAfterSec))')
        block = server[start:server.index('return;', start)]
        self.assertLess(block.index('next();'), block.index('ctx->renewSessionCookie(request)'))

    def test_logout_uses_the_verified_configured_cookie_identity(self):
        source = (ROOT / 'src/network/handlers/AuthRouteHandler.cpp').read_text(encoding='utf8')
        logout = definition(source, 'void AuthRouteHandler::handleLogout(')
        self.assertIn('String sessionId = authResult.sessionId;', logout)
        self.assertNotIn('extractSessionIdFromRequest(request)', logout)

    def test_production_headers_keep_real_cookiejar_active_without_relogin(self):
        result = subprocess.run([str(self.exe), 'emit-cookie'], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        login, renewal = result.stdout.strip().splitlines()
        for interval in (30, 60):
            with self.subTest(interval=interval):
                jar = CookieJar()
                with patch('http.cookiejar.time.time', return_value=1800000000) as now:
                    def accept(header):
                        headers = Message()
                        headers['Set-Cookie'] = header
                        jar.extract_cookies(SimpleNamespace(info=lambda: headers), Request('http://127.0.0.1/api/system/info'))
                    accept(login)
                    for elapsed in range(interval, 9 * 3600 + 1, interval):
                        now.return_value = 1800000000 + elapsed
                        request = Request('http://127.0.0.1/api/system/info')
                        jar.add_cookie_header(request)
                        self.assertEqual(request.get_header('Cookie'), 'sessionId=test-session')
                        accept(renewal)
                    now.return_value += 3600
                    request = Request('http://127.0.0.1/api/system/info')
                    jar.add_cookie_header(request)
                    self.assertIsNone(request.get_header('Cookie'))


if __name__ == '__main__':
    unittest.main()
