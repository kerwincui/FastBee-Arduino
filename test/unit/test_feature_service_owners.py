"""Actual DNS lifecycle and route-owner construction with counted SDK/RTOS mocks."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class FeatureServiceOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / name for name in (
            'src/network/DNSManager.cpp', 'include/network/DNSManager.h',
            'src/network/handlers/UserRouteHandler.cpp', 'include/network/handlers/UserRouteHandler.h',
            'include/core/FeatureInstanceRegistry.h', 'include/core/FeatureObservationCounter.h',
            'test/unit/cpp/feature_service_owners_harness.cpp.in', 'test/unit/test_feature_service_owners.py',
            'include/core/FeatureOwnerLifetime.h',
            'src/network/handlers/LogRouteHandler.cpp', 'include/network/handlers/LogRouteHandler.h')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        dns, header, user, user_header = [path.read_text(encoding='utf-8') for path in cls.inputs[:4]]
        # Compile the entire enabled production DNS branch, including actual
        # methods and synchronization. Substitute only platform includes.
        enabled = dns.split('#if FASTBEE_ENABLE_MDNS\n', 1)[1].split('\n#else\n', 1)[0]
        enabled = enabled.replace('#include <freertos/semphr.h>', '')
        user_start = user.index('FastBee::FeatureObservationCounter<> userAdminOwners;')
        user_end = user.index('bool requireUserAdministration', user_start)
        owners = user[user_start:user_end] + '\n' + '\n'.join(definition(user, signature) for signature in (
            'UserRouteHandler::UserRouteHandler(', 'UserRouteHandler::~UserRouteHandler('))
        template = cls.inputs[6].read_text(encoding='utf-8')
        code = template.replace('// @dns-class@', definition(header, 'class DNSManager') + ';')
        code = code.replace('// @user-class@', definition(user_header, 'class UserRouteHandler') + ';')
        code = code.replace('// @dns@', enabled).replace('// @user@', owners)
        log_source, log_header = [path.read_text(encoding='utf-8') for path in cls.inputs[9:11]]
        code = code.replace('// @log-class@', definition(log_header, 'class LogRouteHandler {') + ';')
        code = code.replace('// @log@', '\n'.join(definition(log_source, signature) for signature in (
            'LogRouteHandler::LogRouteHandler(', 'LogRouteHandler::~LogRouteHandler(')))
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-feature-services-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        cpp = directory / 'services.cpp'
        cpp.write_text(code, encoding='utf-8')
        cls.exe = directory / ('services.exe' if os.name == 'nt' else 'services')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'), str(cpp), '-o', str(cls.exe)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('Service owner input drift')

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('cold', 'mdns_lifecycle', 'mdns_failure', 'mdns_busy', 'mdns_reentrant_observation',
              'mdns_replacement', 'mdns_allocation_retry', 'mdns_concurrent',
              'user_owner_lifetime', 'log_owner_lifetime', 'counter_concurrent', 'counter_overflow', 'execution_unwind',
              'owner_transitions', 'owner_replacement', 'owner_capacity', 'owner_saturation',
              'owner_concurrent', 'owner_unwind'):
    setattr(FeatureServiceOwnerTests, 'test_' + _case, lambda self, mode=_case: self.run_case(mode))


if __name__ == '__main__':
    unittest.main()
