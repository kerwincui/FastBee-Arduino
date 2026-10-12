"""Actual shared PM arbitration, checked motion timers, LEDC ledger and four-pad GPIO.
SDK faults and time are injected; no motor position or physical stop is asserted.
"""
import unittest
from test.unit import test_stepper_managed_lifecycle as lifecycle


class MotionOrdinaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lifecycle.StepperManagedLifecycleTests.setUpClass.__func__(cls)

    check = lifecycle.StepperManagedLifecycleTests.check


CASES = (
    'motion_servo_missing',
    'motion_servo_conflict',
    'motion_servo_invalid_preserves',
    'motion_servo_replace',
    'motion_servo_override',
    'motion_servo_expiry',
    'motion_servo_timer_create',
    'motion_servo_partial_create',
    'motion_servo_timer_start',
    'motion_servo_timer_stop',
    'motion_servo_timer_delete',
    'motion_servo_config',
    'motion_servo_rule',
    'motion_servo_uart',
    'motion_servo_negative_clock',
    'motion_servo_range',
    'motion_servo_no_auto',
    'motion_servo_dispatch',
    'motion_servo_attach_failure',
    'motion_servo_write_failure',
    'motion_servo_release',
    'motion_servo_release_write',
    'motion_servo_release_detach',
    'motion_servo_release_delete',
    'motion_motor_missing',
    'motion_motor_conflict',
    'motion_motor_invalid_preserves',
    'motion_motor_replace',
    'motion_motor_override',
    'motion_motor_expiry',
    'motion_motor_timer_create',
    'motion_motor_partial_create',
    'motion_motor_timer_start',
    'motion_motor_timer_stop',
    'motion_motor_timer_delete',
    'motion_motor_config',
    'motion_motor_rule',
    'motion_motor_uart',
    'motion_motor_negative_clock',
    'motion_motor_idle_speed',
    'motion_motor_dispatch',
    'motion_motor_stop',
    'motion_motor_partial_release',
    'motion_motor_partial_phase',
)
for _case in CASES:
    setattr(MotionOrdinaryTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
