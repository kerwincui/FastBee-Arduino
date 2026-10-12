"""Behavioral regression for source-derived documentation; no build or devices."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert = require('assert/strict'), fs = require('fs'), os = require('os'), path = require('path');
const report = require('./scripts/generate-build-capabilities');
const web = require('./scripts/web-profile');
const ini = fs.readFileSync('platformio.ini', 'utf8');
const minimal = '[env:esp32-F4R0]\nboard = esp32dev\nboard_build.partitions = partitions/4MB.csv\nbuild_flags = -DFASTBEE_ENABLE_MQTT=1\n';
const fixture = (run) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-capability-doc-'));
  try {
    fs.mkdirSync(path.join(root, 'scripts'));
    fs.mkdirSync(path.join(root, 'docs'));
    for (const name of ['platformio.ini', 'scripts/web-profile.js', 'scripts/generate-build-capabilities.js']) fs.copyFileSync(name, path.join(root, name));
    run(root, path.join(root, report.OUTPUT));
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
};
const tests = {
  nine_environments_and_existing_resolver() {
    const value = report.collect(ini);
    assert.equal(value.environments.length, 9);
    assert.equal(value.runtimeVerified, false); assert.equal(value.hardwareVerified, false);
    for (const env of value.environments) {
      const flags = web.resolveBuildFlagsFromIni(ini, env.environment).buildFlags;
      const context = web.resolveWebBuildContext({environment: env.environment, buildFlags: flags});
      assert.equal(env.capabilityFingerprint, context.capabilityFingerprint);
      assert.equal(Object.keys(env.capabilityStates).length, Object.keys(web.CAPABILITY_MACROS).length);
      for (const [key, state] of Object.entries(env.capabilityStates)) assert.equal(state === 'included', context.compiledCapabilities[key]);
    }
  },
  default_check_is_current() { assert.equal(report.checkReport(), true); },
  deterministic_and_line_endings() {
    assert.equal(report.render(report.collect(ini)), report.render(report.collect(ini)));
    assert.equal(report.hash('a\nb\n'), report.hash('\uFEFFa\r\nb\r\n'));
    assert.equal(report.render(report.collect(ini)), report.render(report.collect(ini.replace(/\r?\n/g, '\r\n'))));
  },
  undefined_is_not_firmware_disabled() {
    const env = report.collect(minimal).environments[0];
    assert.equal(env.capabilityStates.mqtt, 'included');
    assert.equal(env.capabilityStates.otaFs, 'not-declared');
    assert.equal(env.capabilityStates.ota, 'not-declared');
    assert(report.render(report.collect(minimal)).includes('不能据此声称 C++ 默认宏'));
  },
  filesystem_ota_declaration_is_not_implementation() {
    for (const declared of ['1','true','on','yes','enabled']) {
      const env = report.collect(minimal.replace('MQTT=1', 'OTA=1 -DFASTBEE_ENABLE_OTA_FS='+declared)).environments[0];
      assert.equal(env.capabilityDeclarations.otaFs, declared);
      assert.equal(env.capabilityStates.otaFs, 'implementation-missing');
      assert.equal(env.capabilityStates.ota, 'included');
      const caps = web.capabilitiesFromBuildFlags(['-DFASTBEE_ENABLE_OTA=1','-DFASTBEE_ENABLE_OTA_FS='+declared]);
      assert.equal(caps.ota, true); assert.equal(caps.otaFs, false);
    }
    const disabled = report.collect(minimal.replace('MQTT=1', 'OTA_FS=0')).environments[0];
    assert.equal(disabled.capabilityStates.otaFs, 'excluded');
  },
  inherited_flags_and_unflags() {
    const source = '[common]\nbuild_flags = -DFASTBEE_ENABLE_MQTT=1 -DFASTBEE_ENABLE_OTA=1\n' +
      minimal.replace('-DFASTBEE_ENABLE_MQTT=1', '$' + '{common.build_flags}') + 'build_unflags = -DFASTBEE_ENABLE_OTA=1\n';
    const env = report.collect(source).environments[0];
    assert.equal(env.capabilityStates.mqtt, 'included'); assert.equal(env.capabilityStates.ota, 'not-declared');
  },
  external_process_overrides_ignored() {
    const expected = report.render(report.collect(ini));
    process.env.FASTBEE_WEB_CAPABILITY_INPUT = 'nonexistent-private-device-input.json';
    process.env.PIOENV = 'not-a-real-env';
    process.argv.push('--capability-input=nonexistent-private-device-input.json');
    assert.equal(report.render(report.collect(ini)), expected);
  },
  duplicate_environment_rejected() { assert.throws(() => report.collect(minimal + minimal), /Duplicate INI/); },
  unknown_environment_rejected() { assert.throws(() => report.collect(minimal.replace('esp32-F4R0', 'vendor-panel')), /Non-canonical/); },
  unsupported_expression_rejected() { assert.throws(() => report.collect(minimal.replace('MQTT=1', 'MQTT=UNKNOWN')), /unsupported capability expression/); },
  unsupported_extends_rejected() { assert.throws(() => report.collect(minimal + 'extends = env:base\n'), /unsupported extends/); },
  unresolved_board_rejected() { assert.throws(() => report.collect(minimal.replace('board = esp32dev', 'board = $' + '{common.board}')), /unresolved\/unsupported board/); },
  stale_check_never_writes() { fixture((root, file) => {
    report.writeReport(root);
    const old = fs.readFileSync(file);
    fs.appendFileSync(path.join(root, 'platformio.ini'), '\n; additional source comment\n');
    assert.throws(() => report.checkReport(root), /missing\/stale/);
    assert.deepEqual(fs.readFileSync(file), old);
  }); },
  missing_check_never_creates() { fixture((root, file) => {
    assert.throws(() => report.checkReport(root), /missing\/stale/); assert(!fs.existsSync(file));
  }); },
  write_refuses_user_document() { fixture((root, file) => {
    fs.writeFileSync(file, 'user-owned content\n');
    assert.throws(() => report.writeReport(root), /Refusing to replace/);
    assert.equal(fs.readFileSync(file, 'utf8'), 'user-owned content\n');
  }); },
  refresh_updates_generated_only() { fixture((root, file) => {
    report.writeReport(root); const old = fs.readFileSync(file, 'utf8');
    fs.appendFileSync(path.join(root, 'platformio.ini'), '\n; changed input\n');
    report.writeReport(root); assert.notEqual(fs.readFileSync(file, 'utf8'), old);
    assert.equal(report.checkReport(root), true);
  }); },
  diagnostic_limits_preserved() {
    const text = report.generate();
    for (const value of ['不是发布成功', '不是设备探测', 'RGB', 'E2～E5', '运行时 features.json']) assert(text.includes(value));
    assert(!text.includes(process.cwd()));
    const matrix = fs.readFileSync('scripts/validate-build-matrix.js', 'utf8');
    assert(matrix.includes("require('./generate-build-capabilities').checkReport(ROOT_DIR)"));
  }
};
tests[process.argv[1]]();
'''


class BuildCapabilityReportTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT, name], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in (
    'nine_environments_and_existing_resolver', 'default_check_is_current',
    'deterministic_and_line_endings', 'undefined_is_not_firmware_disabled', 'filesystem_ota_declaration_is_not_implementation',
    'inherited_flags_and_unflags', 'external_process_overrides_ignored',
    'duplicate_environment_rejected', 'unknown_environment_rejected',
    'unsupported_expression_rejected', 'unsupported_extends_rejected',
    'unresolved_board_rejected', 'stale_check_never_writes', 'missing_check_never_creates',
    'write_refuses_user_document', 'refresh_updates_generated_only', 'diagnostic_limits_preserved',
):
    setattr(BuildCapabilityReportTests, 'test_' + _name,
            lambda self, name=_name: self.run_case(name))


if __name__ == '__main__':
    unittest.main()
