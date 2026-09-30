import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('soil_acceptance', Path(__file__).resolve().parents[2]/'scripts/test-soil-adc-device.py')
soil = importlib.util.module_from_spec(spec); spec.loader.exec_module(soil)

class SoilAcceptanceTests(unittest.TestCase):
    def config(self):
        return {'params':dict(inputOffset=0,inputGain=1,shuntResistance=150,inputMin=4,inputMax=20,
                              engineeringMin=0,engineeringMax=100)}
    def reading(self, **updates):
        row = dict(id='ai_soil_humidity',calibratedVoltage=True,pinVoltageMv=600,
                   quality='valid',valueValid=True,value=0)
        row.update(updates); return row
    def test_valid_zero_moisture_is_accepted(self):
        self.assertEqual(soil.validate_reading('ai_soil_humidity',self.config(),self.reading())['value'],0)
    def test_low_moisture_is_never_a_measurement_pass(self):
        with self.assertRaisesRegex(ValueError,'quality=low'):
            soil.validate_reading('ai_soil_humidity',self.config(),self.reading(quality='low',valueValid=False,value=None,pinVoltageMv=550))
    def test_valid_flag_without_value_fails(self):
        with self.assertRaises(ValueError):
            soil.validate_reading('ai_soil_humidity',self.config(),self.reading(value=None))
    def test_wrong_scaling_fails(self):
        with self.assertRaisesRegex(ValueError,'differs'):
            soil.validate_reading('ai_soil_humidity',self.config(),self.reading(value=5))
    def test_uncalibrated_voltage_fails(self):
        with self.assertRaises(ValueError):
            soil.validate_reading('ai_soil_humidity',self.config(),self.reading(calibratedVoltage=False))
    def test_out_of_range_current_cannot_pass_as_clamped_endpoint(self):
        # A server flag must not override independently checked electrical limits.
        # This is an ADC-pin voltage fixture, not the unverified terminal reading.
        for mv, value in ((9180, 100), (3300, 100), (550, 0), (0, 0)):
            with self.subTest(millivolts=mv):
                with self.assertRaisesRegex(ValueError, 'current.*range'):
                    soil.validate_reading('ai_soil_humidity', self.config(),
                                          self.reading(pinVoltageMv=mv, value=value))
    def test_half_lsb_endpoint_normalization_remains_allowed(self):
        for mv, value in ((599.9, 0), (600, 0), (3000, 100), (3000.1, 100)):
            with self.subTest(millivolts=mv):
                soil.validate_reading('ai_soil_humidity', self.config(),
                                      self.reading(pinVoltageMv=mv, value=value))
    def test_full_lsb_outside_range_is_rejected(self):
        for mv, value in ((599, 0), (3001, 100)):
            with self.subTest(millivolts=mv):
                with self.assertRaises(ValueError):
                    soil.validate_reading('ai_soil_humidity', self.config(),
                                          self.reading(pinVoltageMv=mv, value=value))
    def test_nonfinite_and_boolean_voltage_is_rejected(self):
        for mv in (float('nan'), float('inf'), -1, True):
            with self.subTest(millivolts=mv):
                with self.assertRaises(ValueError):
                    soil.validate_reading('ai_soil_humidity', self.config(), self.reading(pinVoltageMv=mv))
    def test_malformed_calibration_is_rejected(self):
        for field, value in (('shuntResistance', 0), ('inputGain', -1),
                             ('inputOffset', float('nan')), ('inputMax', 4),
                             ('resolution', 13), ('vRef', 0)):
            with self.subTest(field=field):
                config = self.config(); config['params'][field] = value
                with self.assertRaises(ValueError):
                    soil.validate_reading('ai_soil_humidity', config, self.reading())
    def test_current_diagnostics_match_the_reported_adc_pin_voltage(self):
        row = self.reading(adcPin=6, pinVoltageSource='chip_calibrated_adc', calculatedCurrentMa=4)
        soil.validate_reading('ai_soil_humidity', self.config(), row, require_diagnostics=True)
        for changes in ({'adcPin': 7}, {'adcPin': True},
                        {'pinVoltageSource': 'terminal_meter'}, {'calculatedCurrentMa': 20},
                        {'calculatedCurrentMa': None}):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    soil.validate_reading('ai_soil_humidity', self.config(), dict(row, **changes), True)
    def test_post_deploy_gate_rejects_missing_diagnostics(self):
        with self.assertRaises(ValueError):
            soil.validate_reading('ai_soil_humidity', self.config(), self.reading(), True)

if __name__=='__main__': unittest.main()
