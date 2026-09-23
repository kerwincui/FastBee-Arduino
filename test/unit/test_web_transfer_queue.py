import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WebTransferQueueTests(unittest.TestCase):
    def test_chunked_body_timeout_retry_and_navigation_cancellation(self):
        result = subprocess.run(['node', str(ROOT / 'test/unit/node_static_body_queue.cjs')],
                                cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_real_browser_images_share_boot_transport_and_reuse_sidebar_blob(self):
        result = subprocess.run(['node', str(ROOT / 'test/unit/node_boot_image_queue.cjs')],
                                cwd=ROOT, capture_output=True, text=True, timeout=45)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
