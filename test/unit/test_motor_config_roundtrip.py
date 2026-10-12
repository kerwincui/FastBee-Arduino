"""Run the real motor configuration modal without any device requests."""
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const fs = require('fs'), vm = require('vm');
const { minifyJS } = require('./scripts/minify-js');
function probe(minified) {
  const fields = new Map(), warnings = [];
  const element = id => {
    if (!fields.has(id)) fields.set(id, {value:'', checked:false, classList:{remove(){}}});
    return fields.get(id);
  };
  const app = {guardDeveloperModeAction:()=>true, ensureModal:()=>({}), showModal(){}, hideModal(){}, toggleVisible(){}};
  const sandbox = {AppState:app, window:{}, console, document:{getElementById:element}, Notification:{warning:m=>warnings.push(m)}};
  const source = fs.readFileSync('web-src/modules/runtime/protocol/modbus-config.js', 'utf8');
  vm.runInNewContext(minified ? minifyJS(source) : source, sandbox);
  app._renderAllDevices = () => {};
  const zero = {deviceType:'motor', slaveAddress:8, motorRegs:[0,1,2,5,0], motorMinPosition:0,
    motorMaxPosition:0, motorCurrentPosition:0, motorMoveStep:0, motorLastPulse:0, motorDecimals:0};
  app._modbusDevices = [JSON.parse(JSON.stringify(zero))]; app._openEditModal(0);
  const opened = {max:element('mdev-edit-motor-max-pos').value, limits:element('mdev-edit-motor-limits').checked};
  app._saveEditModal(); const savedZero = JSON.parse(JSON.stringify(app._modbusDevices[0]));
  app._modbusDevices = [{...zero, motorMinPosition:-100, motorCurrentPosition:-50}];
  app._openEditModal(0); app._saveEditModal(); const negativeRange = JSON.parse(JSON.stringify(app._modbusDevices[0]));
  app._modbusDevices = []; app._openEditModal(-1); element('mdev-edit-type').value='motor';
  app._saveEditModal(); const newMotor = JSON.parse(JSON.stringify(app._modbusDevices[0]));
  const invalid = [];
  for (const [field,value] of [['max-pos',''],['max-pos','bad'],['max-pos','0.5'],['max-pos','2147483648'],
    ['min-pos','-2147483649'],['pulse','-1'],['fwd','65536'],['min-pos','10']]) {
    app._modbusDevices = [JSON.parse(JSON.stringify(zero))]; app._openEditModal(0);
    element('mdev-edit-motor-limits').checked=true;app._onMotorLimitToggle();
    const before = JSON.stringify(app._modbusDevices); const count=warnings.length;
    element('mdev-edit-motor-'+field).value=value; app._saveEditModal();
    invalid.push(JSON.stringify(app._modbusDevices)===before && warnings.length===count+1 && app._editingDeviceIdx===0);
  }
  app._modbusDevices = [JSON.parse(JSON.stringify(zero))]; app._openEditModal(0);
  element('mdev-edit-motor-limits').checked=true;
  element('mdev-edit-motor-max-pos').value='10'; element('mdev-edit-motor-min-pos').value='20';
  const before = JSON.stringify(app._modbusDevices); app._saveEditModal();
  const outsideRejected = JSON.stringify(app._modbusDevices)===before;
  app._modbusDevices=[]; app._openEditModal(-1); element('mdev-edit-type').value='motor';
  element('mdev-edit-motor-stop').value=''; app._saveEditModal();
  const newInvalidCount=app._modbusDevices.length;
  const profile={version:1,readFunction:4,writeFunction:16,speedReadRegister:0,pulseReadRegister:65535,
    forwardValue:17,reverseValue:34,stopValue:0,finitePulseMove:true};
  const generic={...zero,motorRegs:[100,100,100,101,102],motorProtocol:profile};
  app._modbusDevices=[generic];app._openEditModal(0);app._saveEditModal();
  const genericSaved=JSON.parse(JSON.stringify(app._modbusDevices[0]));
  const invalidProtocol=[];
  for(const [key,value] of [['read-fc','6'],['write-fc','15'],['read-speed','-1'],['read-pulse','65536'],
      ['stop-value','0.2'],['rev-value','17'],['speed','100'],['pulse','101']]) {
    app._modbusDevices=[JSON.parse(JSON.stringify(genericSaved))];app._openEditModal(0);
    const before=JSON.stringify(app._modbusDevices);const n=warnings.length;
    element('mdev-edit-motor-'+key).value=value;app._saveEditModal();
    invalidProtocol.push(before===JSON.stringify(app._modbusDevices)&&warnings.length===n+1);
  }
  app._modbusDevices=[JSON.parse(JSON.stringify(genericSaved))];app._openEditModal(0);
  element('mdev-edit-name').value='Renamed custom motor';
  const unchangedBeforeSave=JSON.stringify(app._modbusDevices[0])===JSON.stringify(genericSaved);
  app._saveEditModal();const editedCustom=JSON.parse(JSON.stringify(app._modbusDevices[0]));
  const legacy=[];
  for(const limited of [false,true]) {
    app._modbusDevices=[{...zero,motorRegs:[0,1,2,5,7],motorMinPosition:limited?-100:0,motorMaxPosition:limited?200:0}];
    app._openEditModal(0);element('mdev-edit-name').value='Renamed legacy motor';
    app._saveEditModal();const ordinary=JSON.parse(JSON.stringify(app._modbusDevices[0]));
    app._openEditModal(0);element('mdev-edit-motor-read-fc').value='4';
    element('mdev-edit-motor-write-fc').value='16';
    element('mdev-edit-motor-read-speed').value='300';element('mdev-edit-motor-read-pulse').value='301';
    app._saveEditModal();const advanced=JSON.parse(JSON.stringify(app._modbusDevices[0]));
    legacy.push({limited,ordinary,advanced});
  }
  return {opened,savedZero,negativeRange,newMotor,invalid,outsideRejected,newInvalidCount,
    genericSaved,invalidProtocol,unchangedBeforeSave,editedCustom,legacy};
}
process.stdout.write(JSON.stringify({source:probe(false),minified:probe(true)}));
"""

class MotorConfigRoundtripTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', timeout=20)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.results = json.loads(result.stdout)

    def test_zero_limits_steps_and_register_addresses_are_preserved(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertEqual({'max':0, 'limits':False}, result['opened'])
                saved = result['savedZero']
                self.assertEqual([0,1,2,5,0], saved['motorRegs'])
                for field in ('motorMinPosition','motorMaxPosition'):
                    self.assertEqual(0, saved[field])
                for field in ('motorCurrentPosition','motorMoveStep','motorLastPulse'):
                    self.assertNotIn(field, saved)

    def test_valid_negative_range_with_zero_upper_bound_is_preserved(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                saved = result['negativeRange']
                self.assertEqual(-100, saved['motorMinPosition'])
                self.assertEqual(0, saved['motorMaxPosition'])
                self.assertNotIn('motorCurrentPosition', saved)

    def test_new_motor_never_enables_an_invented_travel_range(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertEqual(0, result['newMotor']['motorMinPosition'])
                self.assertEqual(0, result['newMotor']['motorMaxPosition'])
                self.assertNotIn('motorMoveStep', result['newMotor'])

    def test_invalid_motor_fields_do_not_mutate_or_create_device_drafts(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertTrue(all(result['invalid']))
                self.assertTrue(result['outsideRejected'])
                self.assertEqual(0, result['newInvalidCount'])

    def test_generic_profile_zero_values_and_custom_feedback_roundtrip(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                saved = result['genericSaved']
                self.assertEqual([100,100,100,101,102], saved['motorRegs'])
                self.assertEqual({'version':1,'readFunction':4,'writeFunction':16,'speedReadRegister':0,
                    'pulseReadRegister':65535,'forwardValue':17,'reverseValue':34,'stopValue':0,
                    'finitePulseMove':True}, saved['motorProtocol'])
                self.assertTrue(all(result['invalidProtocol']))
                self.assertTrue(result['newMotor']['motorProtocol']['finitePulseMove'])

    def test_name_edit_does_not_reset_existing_protocol_identity_or_limits(self):
        for build, result in self.results.items():
            with self.subTest(build=build):
                self.assertTrue(result['unchangedBeforeSave'])
                edited = result['editedCustom']
                self.assertEqual('Renamed custom motor', edited['name'])
                for key in ('motorRegs','slaveAddress','motorMinPosition','motorMaxPosition','motorProtocol'):
                    self.assertEqual(result['genericSaved'][key], edited[key])

    def test_legacy_mapping_is_preserved_until_advanced_settings_change(self):
        for build, result in self.results.items():
            for legacy in result['legacy']:
                with self.subTest(build=build, limited=legacy['limited']):
                    ordinary, advanced = legacy['ordinary'], legacy['advanced']
                    self.assertNotIn('motorProtocol', ordinary)
                    self.assertEqual('Renamed legacy motor', ordinary['name'])
                    self.assertEqual(4, advanced['motorProtocol']['readFunction'])
                    self.assertEqual(16, advanced['motorProtocol']['writeFunction'])
                    self.assertEqual(300, advanced['motorProtocol']['speedReadRegister'])
                    self.assertEqual(301, advanced['motorProtocol']['pulseReadRegister'])
                    for key in ('motorRegs','slaveAddress','motorMinPosition','motorMaxPosition'):
                        self.assertEqual(ordinary[key], advanced[key])

if __name__ == '__main__':
    unittest.main()
