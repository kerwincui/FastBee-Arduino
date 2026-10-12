"""Actual PM Modbus delegates plus typed and named-script producers; no device I/O.

Callbacks and the motor worker are explicit counted seams. Core PM/RTU/worker
admission and ACK history have independent integration suites; these tests
exercise source propagation, lock boundaries, parser rejection and no optimistic
motor/relay cache updates in the actual virtual peripheral implementation.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function
ROOT=Path(__file__).resolve().parents[2]

class ModbusVirtualContextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_header=next(Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps'))).glob('*/ArduinoJson/src/ArduinoJson.h'))
        header=(ROOT/'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        callbacks=header[header.index('using ModbusCoilWriteFunc'):header.index('// 外设配置文件路径')]
        production=(ROOT/'src/core/PeripheralManagerModbus.cpp').read_text(encoding='utf-8')
        production='\n'.join(line for line in production.splitlines() if not line.lstrip().startswith('#include'))
        executor=(ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        typed=cpp_function(executor,'if (digitalTargetFound && digitalTarget.isModbusPeripheral()) {')
        script=cpp_function((ROOT/'src/core/ScriptEngine.cpp').read_text(encoding='utf-8'),
            'if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&\n                    nativeTarget.isModbusPeripheral()) {')
        aggregate='namespace FastBee {\n'+cpp_function(
            (ROOT/'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8'),
            'inline void rememberOrdinaryControlResult(')+'\n}'
        template=(ROOT/'test/unit/cpp/modbus_virtual_context_harness.cpp').read_text(encoding='utf-8')
        source=template.replace('// @callbacks@',callbacks).replace('// @production@',production)
        source=source.replace('// @typed@',typed).replace('// @script@',script).replace('// @aggregate@',aggregate)
        cls.temp=tempfile.TemporaryDirectory(prefix='fastbee-modbus-virtual-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder=Path(cls.temp.name);cpp=folder/'virtual.cpp';cpp.write_text(source,encoding='utf-8')
        # Match ESP32 WString::equalsIgnoreCase's observed C-string loop. The
        # shared host mock compares every byte and otherwise hides NUL acceptance.
        mock=(ROOT/'test/mocks/Arduino.h').read_text(encoding='utf-8')
        comparison=cpp_function(mock,'bool equalsIgnoreCase(const String& other) const {')
        target_comparison='''bool equalsIgnoreCase(const String& other) const {
        if (_value.size() != other._value.size()) return false;
        const char* left = _value.c_str();
        const char* right = other._value.c_str();
        while (*left) {
            if (std::tolower(static_cast<unsigned char>(*left++)) !=
                std::tolower(static_cast<unsigned char>(*right++))) return false;
        }
        return true;
    }'''
        (folder/'Arduino.h').write_text(mock.replace(comparison,target_comparison,1),encoding='utf-8')
        cls.binary=folder/'virtual.exe'
        built=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            '-DARDUINOJSON_ENABLE_STD_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-I'+str(folder),'-I'+str(ROOT/'test/mocks'),'-I'+str(ROOT/'include'),'-I'+str(json_header.parent),
            str(cpp),str(ROOT/'src/network/LiteralStringMatch.cpp'),'-o',str(cls.binary)],capture_output=True,text=True,timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self,scenario):
        run=subprocess.run([str(self.binary),scenario],capture_output=True,text=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)

for _case in ('legacy','pin','register','nc','pwm','pwm_overflow','disabled','stale','uart','busy','failure','cleared',
              'typed_coil','typed_register','typed_reg_negative','typed_overflow','typed_bad_boolean','typed_pwm',
              'typed_nul_true','typed_nul_false',
              'typed_high','typed_unsupported','typed_context','script_high','script_low','script_pwm',
              'script_pwm_invalid','script_unsupported','script_context','script_failure',
              'motor_success','motor_reject','motor_pending','motor_failure','motor_mapping'):
    setattr(ModbusVirtualContextTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__=='__main__':
    unittest.main()
