"""Motor lifecycle against the shared actual arbitration/LEDC/GPIO fixture.
All original scenarios remain. Timer deletion and pad cleanup are SDK acknowledgments.
"""
import unittest
from test.unit import test_stepper_managed_lifecycle as lifecycle


class MotorTickerLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lifecycle.StepperManagedLifecycleTests.setUpClass.__func__(cls)

    def check(self, mode):
        lifecycle.StepperManagedLifecycleTests.check(self, 'motion_legacy_' + mode)


for _mode in ('step', 'reverse', 'stop', 'retire', 'busy', 'replacement', 'deleted', 'pin_changed',
              'suspended', 'runtime_error', 'origin_mismatch', 'origin_invalid', 'malformed', 'unbound',
              'speed_saturate', 'speed_overflow', 'slower', 'speed_idle', 'timer_failed', 'low_memory',
              'token_exhausted', 'invalid_command', 'reserved_pin', 'invalid_pin',
              'servo_release', 'servo_busy', 'servo_replace', 'servo_cancel', 'servo_pin_changed',
              'servo_failed_write', 'servo_failed_detach', 'servo_failed_schedule', 'servo_low_memory',
              'servo_token_exhausted'):
    setattr(MotorTickerLifecycleTests, 'test_'+_mode, lambda self, mode=_mode: self.check(mode))


if __name__ == '__main__':
    unittest.main()
