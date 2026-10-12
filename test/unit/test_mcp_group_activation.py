"""Production group transaction control flow with explicit storage/hardware seams.

Prepared for the final consolidated run. This is not a real MCP/RESET test;
provider, port-session and control-output suites cover those lower layers.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_io_expander_manager import ROOT, function


class McpGroupActivationTests(unittest.TestCase):
    def test_actual_group_transactions(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        code = '\n'.join(function(signature) for signature in (
            'bool PeripheralManager::enableIoExpanderGroupAndSave(',
            'bool PeripheralManager::disableIoExpanderGroupAndSave(',
        ))
        with tempfile.TemporaryDirectory(prefix='fastbee-mcp-group-') as directory:
            temporary = Path(directory)
            (temporary / 'mcp_group_under_test.inc').write_text(code, encoding='utf-8')
            executable = temporary / ('group.exe' if os.name == 'nt' else 'group')
            result = subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                f'-I{temporary}', f'-I{ROOT / "test/mocks"}',
                str(ROOT / 'test/unit/cpp/mcp_group_activation_harness.cpp'),
                '-o', str(executable),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
