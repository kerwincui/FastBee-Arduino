"""Prepared review-token/epoch behavior and API wiring; not device evidence."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from test_io_expander_manager import ROOT, SOURCE, function

ROUTE = (ROOT / 'src/network/handlers/PeripheralRouteHandler.cpp').read_text(encoding='utf-8')


class McpGroupReviewTests(unittest.TestCase):
    def test_actual_epoch_and_token(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        prefix = r'''
#include <Arduino.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "core/SensorConfigEpoch.h"
#define FASTBEE_ENABLE_PERIPH_EXEC 0
#define pdMS_TO_TICKS(x) (x)
bool busy = false;
struct RecursiveMutexGuard {
    RecursiveMutexGuard(int, unsigned) {}
    bool isLocked() const { return !busy; }
};
unsigned entropyCalls = 0;
uint32_t esp_random() { return ++entropyCalls; }
class PeripheralManager {
public:
    int _mutex = 0;
    uint64_t _configurationReviewVersion = 1;
    void invalidateSensorConfigurationLocked();
    uint64_t getConfigurationReviewVersion() const;
};
'''
        main = r'''
int main() {
    PeripheralManager pm;
    const String first = mcpGroupReviewToken(pm.getConfigurationReviewVersion());
    assert(first.length() == 48 && entropyCalls == 4);
    assert(first == mcpGroupReviewToken(pm.getConfigurationReviewVersion()));
    pm.invalidateSensorConfigurationLocked();
    assert(first != mcpGroupReviewToken(pm.getConfigurationReviewVersion()));
    assert(entropyCalls == 4);
    assert(mcpGroupReviewToken(1) != mcpGroupReviewToken(UINT64_C(0x100000001)));
    busy = true; assert(pm.getConfigurationReviewVersion() == 0);
    assert(mcpGroupReviewToken(pm.getConfigurationReviewVersion()).isEmpty()); busy = false;
    pm._configurationReviewVersion = UINT64_MAX;
    pm.invalidateSensorConfigurationLocked(); assert(pm.getConfigurationReviewVersion() == 0);
    pm.invalidateSensorConfigurationLocked(); assert(pm.getConfigurationReviewVersion() == 0);
    assert(mcpGroupReviewToken(0).isEmpty());
}
'''
        code = prefix + '\n'.join((
            function('void PeripheralManager::invalidateSensorConfigurationLocked('),
            function('uint64_t PeripheralManager::getConfigurationReviewVersion('),
            function('String mcpGroupReviewToken(', ROUTE),
        )) + main
        with tempfile.TemporaryDirectory(prefix='fastbee-mcp-review-') as directory:
            path = Path(directory)
            source = path / 'review.cpp'
            source.write_text(code, encoding='utf-8')
            binary = path / ('review.exe' if os.name == 'nt' else 'review')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}',
                                     str(source), '-o', str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mutation_checks_review_under_manager_lock_before_activation(self):
        enable = function('void PeripheralRouteHandler::handleEnablePeripheral(', ROUTE)
        self.assertLess(enable.index('RecursiveMutexGuard managerLock'), enable.index('getConfigurationReviewVersion()'))
        self.assertLess(enable.index('IO_GROUP_REVIEW_STALE'), enable.index('pm.enableIoExpanderGroupAndSave'))
        self.assertIn('ctx->sendError(request, 409', enable)
        self.assertIn('requireAuth(request)', enable)
        self.assertIn('requireDeveloperMode(request)', enable)

    def test_preview_reads_one_locked_snapshot_without_hardware_or_http(self):
        preview = function('bool buildMcpGroupPreview(', ROUTE)
        self.assertLess(preview.index('RecursiveMutexGuard lock'), preview.index('getPeripheralSnapshot'))
        self.assertIn('count > 16 || doc.overflowed()', preview)
        for forbidden in ('initHardware(', 'digitalWrite(', 'sendJson', 'enablePeripheral', 'saveConfiguration'):
            self.assertNotIn(forbidden, preview)

    def test_public_example_is_disabled_and_has_explicit_required_output_owners(self):
        document = json.loads((ROOT / 'docs/mcp23017-disabled-example-20261003.json').read_text(encoding='utf-8'))
        self.assertEqual(set(document), {'peripherals'})
        entries = document['peripherals']
        indexed = {item['id']: item for item in entries}
        self.assertEqual(len(indexed), len(entries))
        for item in entries:
            self.assertIs(item['enabled'], False)
        parent = indexed['mcp_demo']
        self.assertEqual(parent['params']['driver'], 'MCP23017')
        self.assertEqual(parent['pins'], [])
        reset = indexed[parent['params']['resetPeripheralId']]
        self.assertEqual(reset['type'], 12)
        self.assertEqual(reset['params']['ioSource'], 0)
        self.assertEqual(reset['params']['initialState'], 1)
        self.assertIs(reset['params']['invertLogic'], False)
        self.assertIs(reset['params']['passive'], False)
        bus = indexed[parent['params']['busPeripheralId']]
        self.assertEqual(bus['type'], 2)
        self.assertEqual(bus['params']['address'], 0)
        self.assertEqual(len(set(bus['pins'] + reset['pins'])), 3)
        children = [item for item in entries if item['params'].get('providerId') == parent['id']]
        outputs = {item['params']['channel'] for item in children if item['type'] == 12}
        self.assertTrue({7, 15}.issubset(outputs))
        for item in children:
            self.assertEqual(item['pins'], [])
            self.assertEqual(item['params']['ioSource'], 1)
            self.assertEqual(item['params']['inputMode'], 0)


if __name__ == '__main__':
    unittest.main()
