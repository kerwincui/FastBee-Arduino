"""Execute production UART lease and extracted consumer lifecycle methods.

Mocks are SDK/transport dependencies only; no ports or devices are accessed.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition
ROOT=Path(__file__).resolve().parents[2]

def method(source, signature):
    start=source.index(signature)
    return source[start:source.index('\n}', start)+2]

class UartPortLifecycleTests(unittest.TestCase):
    def compile_run(self, template, replacement=None, port_count=3, extra=None, sdk_major=3):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler: self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-uart-lease-') as folder:
            tmp=Path(folder)
            code=(ROOT/'test/unit/cpp'/template).read_text(encoding='utf-8')
            if replacement: code=code.replace('// @production@',replacement)
            cpp=tmp/'harness.cpp'; cpp.write_text(code,encoding='utf-8')
            sources=[str(cpp)]
            if extra:
                peer=tmp/'peer.cpp'; peer.write_text(extra,encoding='utf-8'); sources.append(str(peer))
            binary=tmp/('uart.exe' if os.name=='nt' else 'uart')
            command=[compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                     '-Wno-unused-parameter','-DCHIP_UART_COUNT='+str(port_count),
                     '-DESP_ARDUINO_VERSION_MAJOR='+str(sdk_major),
                     '-I'+str(ROOT/'include'),*sources,'-o',str(binary)]
            built=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=90)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            done=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
            self.assertEqual(done.returncode,0,done.stdout+done.stderr)

    def test_generation_exclusion_cross_translation_unit_and_eight_threads(self):
        self.compile_run('uart_port_lease_harness.cpp',extra='#include "core/UartPortLease.h"\nbool otherTranslationUnitAvailable(uint8_t p) { return FastBee::UartPortLease::available(p); }')

    def test_real_generic_setup_teardown_and_read_lookup(self):
        source='\n'.join((ROOT/name).read_text(encoding='utf-8') for name in (
            'src/core/PeripheralManager.cpp', 'src/core/PeripheralManagerUART.cpp'))
        observer=method(source,'bool installUartReceiveFaultObserver(')
        setup=method(source,'bool PeripheralManager::setupUartHardware(')
        capacity=method(source,'bool PeripheralManager::validateUartCapacity(')
        lookup=method(source,'HardwareSerial* PeripheralManager::getUartSerial(')
        generation=method(source,'uint32_t PeripheralManager::getUartReceiveGeneration(')
        origin=method(source,'bool PeripheralManager::isUartEventOriginCurrent(')
        teardown=method(source,'bool PeripheralManager::teardownHardware(')
        start=teardown.index('    if (config.type == PeripheralType::UART) {')
        end=teardown.index('    if (config.type == ',start+10)
        body='bool PeripheralManager::teardownHardware(const PeripheralConfig& config) {\n'+teardown[start:end]+'return true;\n}'
        for count in (2,3):
            with self.subTest(port_count=count):
                self.compile_run('generic_uart_lifecycle_harness.cpp.in','\n'.join((observer,capacity,setup,lookup,generation,origin,body)),count)
        self.compile_run('generic_uart_lifecycle_harness.cpp.in','\n'.join((observer,capacity,setup,lookup,generation,origin,body)),2,sdk_major=2)

    def test_real_modbus_begin_and_end_preserve_peer_and_busy_owner(self):
        source=(ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        production='\n'.join(method(source,sig) for sig in ('bool ModbusHandler::begin(', 'void ModbusHandler::end()'))
        for count in (2,3):
            with self.subTest(port_count=count):
                self.compile_run('modbus_uart_lifecycle_harness.cpp.in',production,count)

    def test_real_cellular_begin_disconnect_destructor_and_retry(self):
        source=(ROOT/'src/network/CellularAdapter.cpp').read_text(encoding='utf-8')
        production='\n'.join(definition(source,sig) for sig in ('CellularAdapter::CellularAdapter(',
            'CellularAdapter::~CellularAdapter()', 'bool CellularAdapter::begin(', 'void CellularAdapter::disconnect()',
            'Client* CellularAdapter::getClient(', 'Client* CellularAdapter::getSecureClient('))
        for count in (2,3):
            with self.subTest(port_count=count):
                self.compile_run('cellular_uart_lifecycle_harness.cpp.in',production,count)

if __name__=='__main__': unittest.main()
