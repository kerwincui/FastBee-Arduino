"""Run the production login task up to self-deletion without C++ unwinding.

HTTP, authentication and FreeRTOS are host substitutes. The task, job and
result/config declarations are extracted verbatim; String tracks ownership.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition


ROOT = Path(__file__).resolve().parents[2]


class AuthLoginTaskLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/network/handlers/AuthRouteHandler.cpp').read_text(encoding='utf8')
        # An archived pre-fix fragment can be supplied for a negative control.
        # Its subprocess must fail these same ownership checks, not be accepted
        # as a passing regression run.
        override = os.environ.get('FASTBEE_AUTH_LOGIN_TASK_SOURCE')
        task_source = Path(override).read_text(encoding='utf8') if override else source
        result_header = (ROOT / 'include/core/interfaces/IAuthManager.h').read_text(encoding='utf8')
        security_header = (ROOT / 'include/security/AuthManager.h').read_text(encoding='utf8')
        stack = re.search(r'static constexpr uint32_t LOGIN_TASK_STACK_BYTES = \d+U;', source)
        if not stack:
            raise AssertionError('Missing production login stack declaration')
        harness = (ROOT / 'test/unit/cpp/auth_login_task_lifecycle_harness.cpp').read_text(encoding='utf8')
        harness = harness.replace('PRODUCTION_AUTH_RESULT', definition(result_header, 'struct AuthResult') + ';')
        harness = harness.replace('PRODUCTION_SECURITY_CONFIG', definition(security_header, 'struct SecurityConfig') + ';')
        harness = harness.replace('PRODUCTION_STACK', stack.group())
        harness = harness.replace('PRODUCTION_LOGIN_JOB', definition(task_source, 'struct LoginJob') + ';')
        harness = harness.replace('PRODUCTION_LOGIN_TASK', definition(task_source, 'void loginTask('))
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise AssertionError('C++17 compiler required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-login-lifecycle-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cpp = directory / 'login.cpp'
        cpp.write_text(harness, encoding='utf8')
        cls.executable = directory / ('login.exe' if os.name == 'nt' else 'login')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                str(cpp), '-o', str(cls.executable)],
                               capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_case(self, case):
        ran = subprocess.run([str(self.executable), case], capture_output=True,
                             text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('ownership released before task deletion', ran.stdout)

    def test_success_releases_result_before_task_deletion(self):
        self.run_case('success')

    def test_rejection_releases_error_before_task_deletion(self):
        self.run_case('rejected')

    def test_disconnected_client_releases_result_without_response(self):
        self.run_case('disconnected')


if __name__ == '__main__':
    unittest.main()
