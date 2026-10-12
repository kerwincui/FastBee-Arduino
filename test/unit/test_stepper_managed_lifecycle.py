"""Real phase manager methods and ManagedPinIo with error-valued IDF GPIO.

These local substitutions do not measure motor position, current or stopping.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class StepperManagedLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        source = (ROOT / 'src/core/PeripheralManagerStepper.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        helpers = (ROOT / 'src/core/PeripheralManagerConfigHelpers.h').read_text(encoding='utf-8')
        context = cpp_function(header, 'struct StepperTickerData {') + ';'
        servo_context = cpp_function(header, 'struct ServoReleaseTickerData {') + ';'
        action_context = cpp_function(header, 'struct ActionTickerData {') + ';'
        common = (ROOT / 'src/core/PeripheralControlArbitration.cpp').read_text(encoding='utf-8')
        servo = (ROOT / 'src/core/PeripheralManagerServoControl.cpp').read_text(encoding='utf-8')
        pm = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        ledc = (ROOT / 'src/core/PeripheralManagerLedc.cpp').read_text(encoding='utf-8')
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        dispatch = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        helper_code = '\n'.join(cpp_function(config, signature) for signature in (
            'inline bool isServoConfigValid(', 'inline uint16_t servoAngleToPulseUs(',
            'inline uint32_t servoPulseUsToDuty('))
        helper_code += '\nnamespace FastBee {\n' + '\n'.join(cpp_function(dispatch, signature)
            for signature in ('inline bool isOrdinaryScalarTarget(', 'inline void rememberOrdinaryControlResult(',
                              'inline bool rejectOrdinaryDigitalAction(')) + '\n}\n'
        motion_dispatch = (ROOT / 'include/core/MotionOutputDispatch.h').read_text(encoding='utf-8')
        helper_code += '\nnamespace FastBee {\n' + '\n'.join(cpp_function(motion_dispatch, signature)
            for signature in ('inline bool parseServoRawDuty(', 'inline bool parseStepperCommandValue(')) + '\n}\n'
        production = cpp_function(helpers, 'inline uint16_t clampStepperSpeed(') + '\n'
        production += '\n'.join(cpp_function(source, signature) for signature in (
            'uint32_t stepperIntervalMs(', 'void stepperTickerCallback(',
            'bool PeripheralManager::setupStepperHardware(', 'bool PeripheralManager::writeStepperPhase(',
            'bool PeripheralManager::stopStepper(', 'bool PeripheralManager::teardownStepperHardware(',
            'void PeripheralManager::snapshotStepperResources(',
            'void PeripheralManager::serviceStepperTicker(', 'bool PeripheralManager::controlStepper('))
        production = '\n'.join(cpp_function(common, signature) for signature in (
            'bool ordinaryTime(', 'bool ordinaryActorCurrent(', 'bool nativeOrdinaryTarget(',
            'bool ioOrdinaryTarget(', 'void fillResult(',
            'bool PeripheralManager::beginOrdinaryControl(',
            'bool PeripheralManager::ordinaryDigitalClaimedLocked(',
            'bool PeripheralManager::ordinaryPermitCurrentLocked(',
            'bool PeripheralManager::retireOrdinaryDigitalLocked(',
            'bool PeripheralManager::ordinaryBackendSupportedLocked(',
            'bool PeripheralManager::ordinaryProducerPresentLocked(',
            'bool PeripheralManager::ordinaryBackendQuiescentLocked(',
            'void PeripheralManager::updateOrdinaryControlResultLocked(',
            'bool PeripheralManager::completeOrdinaryControlLocked(',
            'bool PeripheralManager::inspectOrdinaryControlLocked(',
            'bool PeripheralManager::commitOrdinaryControlLocked(',
            'void PeripheralManager::finishOrdinaryControl(',
            'void PeripheralManager::serviceOrdinaryControlExpiry(')) + '\n' + production
        production += '\n' + '\n'.join(cpp_function(ledc, signature) for signature in (
            'bool PeripheralManager::attachManagedLedc(', 'bool PeripheralManager::detachManagedLedc('))
        production += '\n' + '\n'.join(cpp_function(pm, signature) for signature in (
            'bool PeripheralManager::attachServoSignal(', 'bool PeripheralManager::setupServoPin('))
        production += '\n' + servo[servo.index('bool PeripheralManager::writeServoAngle('):]
        motion_cases = (CPP / 'motion_ordinary_cases.cpp.in').read_text(encoding='utf-8')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        script = (ROOT / 'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        for marker, source_text, signature in (
            ('// @typed_servo@', executor, 'if (digitalTargetFound && digitalTarget.type == PeripheralType::PWM_SERVO) {'),
            ('// @call_stepper@', executor, 'if (targetCfg->type == PeripheralType::STEPPER_MOTOR) {'),
            ('// @call_servo@', executor, 'if (targetCfg->type == PeripheralType::PWM_SERVO) {'),
            ('// @script_servo@', script, 'if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&\n                    nativeTarget.type == PeripheralType::PWM_SERVO) {')):
            motion_cases = motion_cases.replace(marker, cpp_function(source_text, signature))
        harness = (CPP / 'stepper_managed_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-stepper-managed-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        unit = folder / 'stepper.cpp'
        timer = (CPP / 'ordinary_control_mocks/esp_timer.h').read_text(encoding='utf-8')
        timer = timer.replace('using esp_err_t = int;', '#include <driver/gpio.h>').replace(
            'constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;', '')
        (folder / 'esp_timer.h').write_text(timer, encoding='utf-8')
        harness = harness.replace('// @context@', context).replace('// @servo_context@', servo_context)
        harness = harness.replace('// @action_context@', action_context).replace('// @helpers@', helper_code)
        harness = harness.replace('// @motion_cases@', motion_cases).replace('// @production@', production)
        unit.write_text(harness, encoding='utf-8')
        cls.binaries = {}
        # The actual checked GPIO implementation is compiled in a separate TU.
        for chip in ('ESP32S3', 'ESP32C6'):
            binary = folder / (chip + '.exe')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1', '-DARDUINO_ISR_FLAG=0',
                # Preserve the full Arduino mock while supplying the checked
                # IDF GPIO declarations to both this harness and ManagedPinIo TU.
                '-include', str(CPP / 'managed_pin_mocks/driver/gpio.h'),
                '-I' + str(folder), '-I' + str(ROOT / 'test/mocks'), '-I' + str(CPP / 'managed_pin_mocks'),
                '-I' + str(ROOT / 'include'), str(unit), str(ROOT / 'src/core/ManagedPinIo.cpp'),
                '-o', str(binary)], capture_output=True, text=True, timeout=90)
            if build.returncode:
                raise AssertionError(f'{chip} exit={build.returncode}\n{build.stdout or ""}{build.stderr or ""}')
            cls.binaries[chip] = binary

    def check(self, mode):
        for chip, binary in self.binaries.items():
            with self.subTest(chip=chip):
                run = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, 0,
                                 f'{chip} exit={run.returncode}\n{run.stdout or ""}{run.stderr or ""}')


CASES = ('lifecycle', 'passive_snapshot', 'busy_snapshot', 'duplicate_pin', 'invalid_pin',
         'reserved_pin', 'input_only', 'adc_owned', 'ledc_owned', 'control_owned',
         'other_config', 'gpio_owned', 'foreign_bus', 'low_memory', 'id_too_long', 'neopixel_owned',
         'repeated_setup', 'changed_config', 'retired_origin', 'timer_failed',
         'disabled_stop', 'lost_periman', 'snapshot_truncated', 'speed_idle')
CASES += tuple(f'{operation}_{position}' for operation in
               ('setup_level', 'setup_config', 'phase', 'release_level', 'release_config')
               for position in range(1, 5))
for _case in CASES:
    setattr(StepperManagedLifecycleTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
