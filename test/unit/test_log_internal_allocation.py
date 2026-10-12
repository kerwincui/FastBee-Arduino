"""Exercise real handler allocator and the initialization failure block."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT=Path(__file__).resolve().parents[2]

class LogInternalAllocationTests(unittest.TestCase):
    def test_real_allocator_caps_and_cold_start_oom_before_constructor(self):
        source=(ROOT/'src/network/handlers/LogRouteHandler.cpp').read_text(encoding='utf-8')
        manager=(ROOT/'src/network/WebConfigManager.cpp').read_text(encoding='utf-8')
        template=(ROOT/'test/unit/cpp/log_internal_allocation_harness.cpp.in').read_text(encoding='utf-8')
        begin=manager.index('    logHandler         =')
        block=manager[begin:manager.index('#endif',begin)]
        for key,value in {
            '@ALLOCATE@':definition(source,'void* LogRouteHandler::operator new('),
            '@DELETE@':definition(source,'void LogRouteHandler::operator delete(void* storage)'),
            '@SIZED_DELETE@':definition(source,'void LogRouteHandler::operator delete(void* storage, size_t bytes)'),
            '@INITIALIZE@':block,
        }.items():template=template.replace(key,value)
        # Also verify both actual nullable paths, outside the cold-start block.
        self.assertIn('if (logHandler) logHandler->setupRoutes(server)',manager)
        self.assertIn('if (logHandler) logHandler->performMaintenance()',manager)
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler,'A C++17 compiler is required')
        with tempfile.TemporaryDirectory(prefix='fastbee-log-internal-') as directory:
            cpp=Path(directory)/'internal.cpp';cpp.write_text(template,encoding='utf-8')
            executable=Path(directory)/('internal.exe' if os.name=='nt' else 'internal')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(executable)],
                                  capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
