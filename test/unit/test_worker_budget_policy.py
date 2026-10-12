"""Compile production worker budget/lifecycle and network baseline lease."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT=Path(__file__).resolve().parents[2]
class WorkerBudgetPolicyTests(unittest.TestCase):
    def test_actual_policy_pool_exit_growth_and_network_baseline(self):
        strip=lambda t:'\n'.join(l for l in t.splitlines() if not l.lstrip().startswith(('#include','#pragma')))
        manager=(ROOT/'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf8')
        network=(ROOT/'include/network/NetworkManager.h').read_text(encoding='utf8')
        production={
            '@POLICY@':(ROOT/'include/core/WorkerPoolBudget.h').read_text(encoding='utf8'),
            '@POOL_HEADER@':strip((ROOT/'include/core/PeriphExecWorkerPool.h').read_text(encoding='utf8')),
            '@POOL_SOURCE@':strip((ROOT/'src/core/PeriphExecWorkerPool.cpp').read_text(encoding='utf8')),
            '@LIFETIME@':strip((ROOT/'include/protocols/MqttClientLease.h').read_text(encoding='utf8')),
            '@NETWORK_API@':definition(network,'FastBee::MqttClientLifetime::Read acquireWorkerBudgetBaselineRead()'),
            '@BASELINE@':definition(manager,'static FastBee::MqttClientLifetime::Read acquireWorkerNetworkBaseline()'),
            '@ADMISSION@':definition(manager,'static bool hasWorkerPoolMemoryHeadroom('),
        }
        fixture=(ROOT/'test/unit/cpp/worker_budget_harness.cpp.in').read_text(encoding='utf8')
        for marker,value in production.items():
            self.assertEqual(fixture.count(marker),1,marker)
            fixture=fixture.replace(marker,value)
        compiler=shutil.which('g++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='fastbee-worker-budget-') as directory:
            cpp,exe=Path(directory)/'pool.cpp',Path(directory)/'pool.exe'
            cpp.write_text(fixture,encoding='utf8')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],
                                  capture_output=True,text=True,timeout=60,env=os.environ.copy())
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
