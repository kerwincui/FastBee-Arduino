"""Run the production directory reader against oversized, slow and pressured storage."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]

class LogDirectoryBudgetTests(unittest.TestCase):
    def run_reader(self, baseline=False, regression_only=False, slow_r5=False, operation_r6=False):
        source = (ROOT/'src/network/handlers/LogRouteHandler.cpp').read_text(encoding='utf-8')
        template = (ROOT/'test/unit/cpp/log_directory_budget_harness.cpp.in').read_text(encoding='utf-8')
        # Freeze each historical reader together with its actual budget class.
        # Borrowing the current class changes the old failure being reproduced.
        historical = None
        if baseline:
            filename = ('log_directory_r6_before.inc' if baseline == 'r6' else
                        'log_directory_r5_before.inc' if baseline == 'r5' else
                        'log_directory_metadata_before.inc')
            historical = (ROOT/'test/unit/cpp'/filename).read_text(encoding='utf-8')
        replacements = {
            '@KIND@': definition(source, 'enum class LogReadKind')+';',
            '@CONSTANTS@': source[source.index('constexpr uint32_t LOG_READ_STACK_BYTES'):source.index('class LogDirectoryScanBudget')],
            '@SCAN@': definition(historical or source, 'class LogDirectoryScanBudget')+';',
            '@PEAK@': definition(source, 'uint32_t logReadPeakBytes('),
            '@LIVE@': definition(source, 'bool logClientLive('),
            '@ERROR@': definition(source[source.rindex('void sendLogReadError('):], 'void sendLogReadError('),
            '@BUDGET@': definition(source, 'bool requireLogReadBudget('),
            '@ACCESS@': definition(source, 'bool requireLogFileAccess('),
            '@READER@': definition(historical or source, 'void readLogList('),
        }
        for key,value in replacements.items(): template = template.replace(key,value)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler,'A C++17 compiler is required')
        with tempfile.TemporaryDirectory(prefix='fastbee-log-directory-') as directory:
            cpp = Path(directory)/'directory.cpp'; cpp.write_text(template,encoding='utf-8')
            executable = Path(directory)/('directory.exe' if os.name=='nt' else 'directory')
            flags=['-DFASTBEE_LOG_METADATA_REGRESSION_ONLY'] if regression_only else []
            if slow_r5: flags=['-DFASTBEE_LOG_SEGMENTED_REGRESSION_ONLY']
            if operation_r6: flags=['-DFASTBEE_LOG_OPERATION_REGRESSION_ONLY']
            result = subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(cpp),'-o',str(executable)],
                                    capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result = subprocess.run([str(executable)],capture_output=True,text=True,timeout=10)
            if baseline:
                self.assertNotEqual(result.returncode,0,'Previous production reader must reject the 21-file fixture')
                self.assertIn('request.status==200',result.stderr)
                if operation_r6:
                    self.assertIn('ACTUAL status=503 entries=0 readdir=1 stat=1 clock=129 yields=0 rootsClosed=1',result.stderr)
            else:
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_actual_directory_reader_bounds_and_resource_rechecks(self):
        self.run_reader()

    def test_actual_before_reader_reproduces_21_file_open_cost_failure(self):
        self.run_reader(baseline=True,regression_only=True)

    def test_actual_metadata_reader_accepts_complete_21_file_collection(self):
        self.run_reader(regression_only=True)

    def test_actual_r5_reader_reproduces_whole_deadline_failure(self):
        self.run_reader(baseline='r5',slow_r5=True)

    def test_actual_segmented_reader_accepts_slow_complete_21_file_collection(self):
        self.run_reader(slow_r5=True)

    def test_actual_r6_batch_budget_reproduces_129ms_cumulative_failure(self):
        self.run_reader(baseline='r6',operation_r6=True)

    def test_actual_individual_vfs_budget_accepts_129ms_and_wrap(self):
        self.run_reader(operation_r6=True)

if __name__=='__main__': unittest.main()
