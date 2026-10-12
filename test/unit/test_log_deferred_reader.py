"""Extract production deferred-log worker, reaper and mutex guard for lifecycle faults."""
from pathlib import Path
import os
import subprocess
import shutil
import tempfile
import unittest

from test_sensor_publication import definition

ROOT=Path(__file__).resolve().parents[2]

class LogDeferredReaderTests(unittest.TestCase):
    def test_real_worker_admission_lock_disconnect_cleanup_and_retirement(self):
        source=(ROOT/'src/network/handlers/LogRouteHandler.cpp').read_text(encoding='utf-8')
        worker=source[source.index('using LogFileOperation'):source.index('class ScopedLogFileAccess')]
        template=(ROOT/'test/unit/cpp/log_deferred_reader_harness.cpp.in').read_text(encoding='utf-8')
        replacements={
            '@WORKER@':worker,
            '@GUARD@':definition(source,'class ScopedLogFileAccess')+';',
            '@ACCESS@':definition(source,'bool requireLogFileAccess('),
            '@MAINTENANCE@':definition(source,'void LogRouteHandler::performMaintenance('),
            '@DESTRUCTOR@':definition(source,'LogRouteHandler::~LogRouteHandler('),
        }
        for key,value in replacements.items():template=template.replace(key,value)
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler,'A C++17 compiler is required')
        with tempfile.TemporaryDirectory(prefix='fastbee-log-deferred-') as directory:
            cpp=Path(directory)/'worker.cpp'
            cpp.write_text(template,encoding='utf-8')
            executable=Path(directory)/('worker.exe' if os.name=='nt' else 'worker')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                str(cpp),'-o',str(executable)],capture_output=True,text=True,
                encoding='utf-8',errors='replace',timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':
    unittest.main()
