"""Compile the actual lazy queue owner and preserve OTA admission/reply contracts."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaReplyQueueLifetimeTests(unittest.TestCase):
    def test_actual_owner_cold_oom_inflight_retirement_and_concurrency(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-ota-queues-') as directory:
            executable = Path(directory) / ('queues.exe' if os.name == 'nt' else 'queues')
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'test/unit/cpp/ota_queue_mocks'), '-I' + str(ROOT / 'include'),
                str(ROOT / 'test/unit/cpp/ota_reply_queue_lifetime_harness.cpp'), '-o', str(executable)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('OTA reply queue lifecycle checks passed', ran.stdout)

    def test_flash_admission_and_cleanup_keep_reply_ownership(self):
        source = (ROOT / 'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        command = source.split('void ProtocolManager::handleFastBeeOtaCommand(', 1)[1].split(
            'void ProtocolManager::queueFastBeeOtaReply(', 1)[0]
        self.assertLess(command.index('getIsConnected()'), command.index('fastBeeOtaReplyQueues.ensureAndAcquire()'))
        self.assertLess(command.index('hasUsableFastBeeOtaReplyTopic()'), command.index('fastBeeOtaReplyQueues.ensureAndAcquire()'))
        self.assertLess(command.index('if (!replyStorage)'), command.index('fastBeeOtaActiveTaskId = command.taskId'))
        self.assertLess(command.index('if (!replyStorage)'), command.index('startOTAAsync('))
        process = source.split('void ProtocolManager::processFastBeeOtaReplies()', 1)[1].split('\n#endif', 1)[0]
        self.assertLess(process.index('releaseIfIdle'), process.index('acquireMQTTClient()'))
        self.assertIn('fastBeeOtaActiveTaskId == 0', process)
        self.assertIn('!fastBeeOtaHasPendingReply', process)
        self.assertIn('replyStorage.terminal()', process)
        self.assertIn('replyStorage.replies()', process)
        self.assertIn('replyStorage.progress()', process)
        constructor = source.split('ProtocolManager::ProtocolManager(', 1)[1].split('ProtocolManager::~ProtocolManager(', 1)[0]
        self.assertNotIn('xQueueCreate', constructor)


if __name__ == '__main__':
    unittest.main()
