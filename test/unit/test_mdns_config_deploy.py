"""Offline, fail-closed mDNS-only deployment contracts; no serial access."""
import importlib.util
import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest import mock
from littlefs import LittleFS

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('disable_mdns', ROOT/'scripts/disable-device-mdns.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
class OfflineReader:
    """Independent reader keeps unit tests free of ignored private artifacts."""
    @staticmethod
    def read_files(image):
        fs=LittleFS(block_size=4096,block_count=len(image)//4096,mount=False)
        fs.context.buffer=bytearray(image); fs.mount()
        files={}; directories=set()
        try:
            for root, dirs, names in fs.walk('/'):
                for name in dirs: directories.add((root+'/'+name).lstrip('/'))
                for name in names:
                    path=(root+'/'+name).lstrip('/')
                    with fs.open('/'+path,'rb') as stream: files[path]=stream.read()
        finally: fs.unmount()
        return files,directories


helper=OfflineReader()


def fixture():
    return {
        'config/network.json': json.dumps(dict(enableMDNS=True, ssid='test-ssid',
            password='synthetic-only', customDomain='preserved', useStaticIP=True)).encode(),
        'config/features.json': json.dumps(dict(version=1, features={
            'mdns': {'enabled': True, 'custom': 'preserved'},
            'webServer': {'enabled': True}, 'lcd': {'enabled': True},
            'mqtt': {'enabled': False}})).encode(),
        'config/users.json': b'{"test":"synthetic-only"}',
        'config/peripherals.json': b'{"devices":[1,2,3]}',
        'config/rules.json': b'{"rules":[1,2]}',
        'www/index.html.gz': bytes(range(256)),
    }


def image_for(contents):
    fs = LittleFS(block_size=4096, block_count=module.FS_BYTES//4096,
                  read_size=1, prog_size=1, cache_size=4096, lookahead_size=32,
                  block_cycles=500, name_max=64, disk_version=0x00020001)
    for directory in ['/config','/www','/history','/history/empty']:
        fs.mkdir(directory)
    for path, data in contents.items():
        with fs.open('/'+path, 'wb') as stream:
            stream.write(data)
    fs.unmount()
    return bytes(fs.context.buffer)


class MdnsConfigDeployTests(unittest.TestCase):
    def test_esptool_default_uses_current_user_profile(self):
        source = Path(module.__file__).read_text(encoding='utf-8')
        self.assertIn("Path.home() / '.platformio/packages/tool-esptoolpy'", source)
        self.assertNotIn('C:' + '/Users/', source)

    def test_flash_read_block_checks_stream_and_independent_md5(self):
        data=b'x'*2048
        esp=mock.Mock(); esp.ESP_CMDS={'READ_FLASH':210}
        esp.read.side_effect=[data[:1024],data[1024:],hashlib.md5(data).digest()]
        esp.flash_md5sum.return_value=hashlib.md5(data).hexdigest()
        self.assertEqual(module.read_flash_block(esp,0x10000,len(data),'SYNTHETIC'),data)
        self.assertEqual(esp.write.call_count,2)
        esp.flash_md5sum.assert_called_once_with(0x10000,2048)

    def test_flash_read_rejects_bad_length_without_acknowledging(self):
        esp=mock.Mock();esp.ESP_CMDS={'READ_FLASH':210};esp.read.return_value=b'x'*16
        with self.assertRaisesRegex(ValueError,'0x10000: expected 1024, received 16'):
            module.read_flash_block(esp,0x10000,2048,'SYNTHETIC')
        esp.write.assert_not_called();esp.flash_md5sum.assert_not_called()

    def test_flash_read_rejects_either_digest_mismatch(self):
        for stream_good in [False,True]:
            with self.subTest(stream_good=stream_good):
                data=b'x'*1024;esp=mock.Mock();esp.ESP_CMDS={'READ_FLASH':210}
                esp.read.side_effect=[data,hashlib.md5(data).digest() if stream_good else b'0'*16]
                esp.flash_md5sum.return_value='0'*32
                with self.assertRaisesRegex(ValueError,'MD5 mismatch'):
                    module.read_flash_block(esp,0,1024,'SYNTHETIC')

    def test_changes_only_two_boolean_fields(self):
        original = fixture()
        result, changed = module.disabled_configs(original)
        self.assertEqual(set(changed), set(module.CONFIG_KEYS))
        for path, data in original.items():
            if path not in module.CONFIG_KEYS:
                self.assertEqual(data, result[path])
                continue
            restored = json.loads(result[path])
            cursor = restored
            for key in module.CONFIG_KEYS[path][:-1]:
                cursor = cursor[key]
            self.assertIs(cursor[module.CONFIG_KEYS[path][-1]], False)
            cursor[module.CONFIG_KEYS[path][-1]] = True
            self.assertEqual(restored, json.loads(data))
        self.assertEqual(original, fixture())

    def test_one_flag_already_disabled_stays_byte_identical(self):
        original = fixture()
        original['config/network.json'] = b' {"enableMDNS":false,"keep":42}  '
        result, changed = module.disabled_configs(original)
        self.assertEqual(changed, ['config/features.json'])
        self.assertEqual(result['config/network.json'], original['config/network.json'])

    def test_missing_files_fail_closed(self):
        for path in module.CONFIG_KEYS:
            with self.subTest(path=path):
                data = fixture(); del data[path]
                with self.assertRaises(ValueError): module.disabled_configs(data)

    def test_schema_type_and_duplicate_keys_fail_closed(self):
        invalid = [b'{}', b'[]', b'{"enableMDNS":1}', b'{"enableMDNS":"false"}',
                   b'{"enableMDNS":null}', b'{"enableMDNS":true,"enableMDNS":false}']
        for payload in invalid:
            with self.subTest(payload=payload):
                data = fixture(); data['config/network.json'] = payload
                with self.assertRaises(ValueError): module.disabled_configs(data)
        for value in [None, 1, [], 'enabled', {'mdns': []}, {'mdns': {'enabled': 'false'}}]:
            with self.subTest(value=value):
                data = fixture(); data['config/features.json'] = json.dumps({'features': value}).encode()
                with self.assertRaises(ValueError): module.disabled_configs(data)

    def test_littlefs_roundtrip_preserves_files_and_empty_directories(self):
        original = image_for(fixture())
        candidate, report = module.patch_image(original, helper)
        before, dirs = helper.read_files(original)
        after, after_dirs = helper.read_files(candidate)
        self.assertEqual(after, module.disabled_configs(before)[0])
        self.assertEqual(dirs, after_dirs)
        self.assertIn('history/empty', after_dirs)
        self.assertEqual(len(original), len(candidate))
        self.assertTrue(report['verified'])

    def test_second_patch_is_byte_identical_noop(self):
        once, _ = module.patch_image(image_for(fixture()), helper)
        twice, report = module.patch_image(once, helper)
        self.assertEqual(once, twice)
        self.assertTrue(report['noOp'])
        self.assertEqual(report['changedFiles'], [])

    def test_every_prewrite_gate_is_mandatory(self):
        gates = dict.fromkeys(['identityVerified','backupVerified','applicationVerified',
                              'sourceStable','candidateVerified'], True)
        module.verify_write_gate(gates)
        for key in gates:
            for value in [False, None, 1, 'true']:
                with self.subTest(key=key, value=value):
                    broken = dict(gates, **{key: value})
                    with self.assertRaises(ValueError): module.verify_write_gate(broken)

    def test_no_hardware_without_explicit_write(self):
        args = ['deploy', '--port','COM4','--output','unused','--gates','unused']
        with mock.patch.object(sys, 'argv', args), mock.patch.object(module, 'require_lock') as lock:
            with self.assertRaisesRegex(ValueError, 'Explicit --write'): module.main()
            lock.assert_not_called()

    def test_ports_are_restricted(self):
        self.assertEqual(set(module.TARGETS), {'COM4', 'COM16'})
        for port in ['COM8', 'COM15']:
            args = ['deploy','--port',port,'--output','unused','--gates','unused','--write']
            with mock.patch.object(sys,'argv',args), mock.patch('sys.stderr'), \
                    mock.patch.object(module,'require_lock') as lock:
                with self.assertRaises(SystemExit): module.main()
                lock.assert_not_called()

    def test_bounded_capture_required_before_device_access(self):
        args = ['deploy','--port','COM4','--output','unused','--gates','unused','--write']
        for duration in ['0','301']:
            with mock.patch.object(sys,'argv',args+['--capture-seconds',duration]), \
                    mock.patch.object(module,'require_lock') as lock:
                with self.assertRaises(ValueError): module.main()
                lock.assert_not_called()

    def test_lock_token_cannot_be_absent(self):
        fake = json.dumps({'token':'actual','projectRoot':str(ROOT),'pid':1,'processStartTicks':'1'})
        with mock.patch.object(Path,'read_text',return_value=fake), mock.patch.dict(module.os.environ, {}, clear=True):
            with self.assertRaises(ValueError): module.require_lock()

    def test_reset_attempt_is_recorded_before_reset_failure(self):
        esp=mock.Mock(); esp.hard_reset.side_effect=RuntimeError('synthetic reset failure')
        report = {'resetRequested':False}
        with self.assertRaises(RuntimeError): module.capture_after_reset(esp,Path('unused'),60,report)
        self.assertTrue(report['resetRequested'])
        self.assertNotIn('resetCompleted', report)
        esp.hard_reset.assert_called_once()


if __name__ == '__main__':
    unittest.main(verbosity=2)
