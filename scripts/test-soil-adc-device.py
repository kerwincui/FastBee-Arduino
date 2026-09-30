"""Read-only soil ADC acceptance; invalid readings fail, even when Modbus passes.

Credentials use FASTBEE_USERNAME/FASTBEE_PASSWORD. Temperature reference is an
observation supplied by the operator, never a fitted gain or offset.
"""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import statistics
import time

EXPECTED = {'ai_soil_humidity': (6, 0, 100), 'ai_soil_temp': (7, -40, 80)}

def validate_config(ident, config):
    pin, low, high = EXPECTED[ident]
    p = config.get('params', {})
    checks = [config.get('id') == ident, config.get('type') == 26,
              config.get('pins') == [pin], config.get('enabled') is True,
              p.get('calibratedVoltage') is True, p.get('conversionEnabled') is True,
              p.get('signalMode') == 3, p.get('shuntResistance') == 150,
              p.get('inputMin') == 4, p.get('inputMax') == 20,
              p.get('engineeringMin') == low, p.get('engineeringMax') == high,
              p.get('inputGain') == 1, p.get('inputOffset') == 0,
              p.get('engineeringOffset') == 0, p.get('clampEnabled') is False]
    if not all(checks):
        raise ValueError('Unexpected soil ADC configuration: ' + ident)

def validate_reading(ident, config, reading, require_diagnostics=False):
    mv = reading.get('pinVoltageMv')
    if reading.get('id') != ident or reading.get('calibratedVoltage') is not True:
        raise ValueError('Missing calibrated voltage diagnostics: ' + ident)
    if type(mv) not in (int, float) or not math.isfinite(mv) or mv < 0:
        raise ValueError('Invalid calibrated voltage: ' + ident)
    p = config['params']
    for name in ('inputOffset', 'inputGain', 'shuntResistance', 'inputMin', 'inputMax',
                 'engineeringMin', 'engineeringMax'):
        v = p.get(name)
        if type(v) not in (int, float) or not math.isfinite(v):
            raise ValueError('Invalid analog configuration: ' + name)
    if p['inputGain'] <= 0 or p['shuntResistance'] <= 0 or p['inputMax'] <= p['inputMin']:
        raise ValueError('Invalid analog configuration range or gain')
    current = (mv / 1000 - p['inputOffset']) / p['inputGain'] * 1000 / p['shuntResistance']
    diagnostic_fields = ('adcPin', 'pinVoltageSource', 'calculatedCurrentMa')
    if require_diagnostics or any(k in reading for k in diagnostic_fields):
        if type(reading.get('adcPin')) is not int or reading['adcPin'] != EXPECTED[ident][0]:
            raise ValueError('ADC diagnostic pin mismatch: ' + ident)
        if reading.get('pinVoltageSource') != 'chip_calibrated_adc':
            raise ValueError('ADC voltage provenance mismatch: ' + ident)
        diagnostic_current = reading.get('calculatedCurrentMa')
        if (type(diagnostic_current) not in (int, float) or not math.isfinite(diagnostic_current) or
                not math.isclose(diagnostic_current, current, rel_tol=1e-5, abs_tol=.0001)):
            raise ValueError('ADC diagnostic current mismatch: ' + ident)
    low, high = p['engineeringMin'], p['engineeringMax']
    expected = low + (current - p['inputMin']) / (p['inputMax'] - p['inputMin']) * (high-low)
    value = reading.get('value')
    if reading.get('quality') != 'valid' or reading.get('valueValid') is not True:
        raise ValueError('%s: quality=%s, voltage=%smV, current=%.5fmA' %
                         (ident, reading.get('quality'), mv, current))
    # Independently verify the physical range before normalizing endpoints.
    # Previously any voltage could pass if the server claimed valid and returned
    # the clipped engineering endpoint (including zero or 9.18V at the ADC pin).
    resolution = p.get('resolution', 12)
    if type(resolution) is not int or not 9 <= resolution <= 12:
        raise ValueError('Invalid ADC resolution')
    vref = p.get('vRef', 3.3)
    if type(vref) not in (int, float) or not math.isfinite(vref) or vref <= 0:
        raise ValueError('Invalid ADC reference voltage')
    tolerance = vref / ((1 << resolution) - 1) / p['inputGain'] * 1000 / p['shuntResistance'] * .5
    if not math.isfinite(current) or not p['inputMin'] - tolerance <= current <= p['inputMax'] + tolerance:
        raise ValueError('%s: current %.5fmA outside configured range' % (ident, current))
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError('Valid quality has no numeric measurement: ' + ident)
    # Only accommodate the firmware half-LSB normalization at exact endpoints.
    expected = max(low, min(high, expected))
    if not math.isclose(value, expected, abs_tol=.001):
        raise ValueError('Engineering value differs from its calibrated voltage: ' + ident)
    return dict(currentMa=current, expected=expected, value=value)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--expected-mac', required=True)
    parser.add_argument('--samples', type=int, default=12)
    parser.add_argument('--interval', type=float, default=3)
    parser.add_argument('--reference-temperature', type=float)
    parser.add_argument('--require-current-diagnostics', action='store_true',
                        help='Require ADC pin, calibrated-voltage source, and calculated-current fields')
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    if not 1 <= args.samples <= 600 or not 1 <= args.interval <= 60:
        raise ValueError('Invalid observation bounds')
    out = Path(args.out); out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists(): raise ValueError('Refusing to replace existing evidence')
    report = dict(at=datetime.now(timezone.utc).isoformat(), status='FAIL', samples=[], errors=[],
                  statusScope='configuration-and-signal-conversion-only',
                  measurementAccuracyStatus='NOT_VERIFIED', target=args.url, expectedMac=args.expected_mac,
                  unverified=['physical-moisture-accuracy', 'traceable-temperature-calibration', 'long-duration-soak'])
    try:
        import requests
        session = requests.Session(); session.trust_env = False
        password = os.environ['FASTBEE_PASSWORD']
        r = session.post(args.url.rstrip('/')+'/api/auth/login',
                         data=dict(username=os.environ.get('FASTBEE_USERNAME','admin'),password=password),
                         timeout=(3,12))
        r.raise_for_status()
        if r.json().get('success') is not True: raise ValueError('Authentication failed')
        def api(route):
            r = session.get(args.url.rstrip('/')+route, timeout=(3,12)); r.raise_for_status()
            body = r.json()
            if body.get('success') is not True: raise ValueError('API failed: '+route)
            return body.get('data') or {}
        info = api('/api/system/info')
        norm = lambda s: re.sub('[^0-9a-f]', '', s.lower())
        if len(norm(args.expected_mac)) != 12 or norm(info.get('network',{}).get('macAddress','')) != norm(args.expected_mac):
            raise ValueError('Device identity mismatch')
        report['actualMac'] = info['network']['macAddress']
        report['identityVerified'] = True
        configs = {ident:api('/api/peripherals/?id='+ident) for ident in EXPECTED}
        for ident, config in configs.items(): validate_config(ident, config)
        report['configs'] = configs
        previous = info['uptime']['seconds']
        report['initialUptime'] = previous
        for index in range(args.samples):
            row = dict(index=index, sampledAt=datetime.now(timezone.utc).isoformat(), readings={})
            for ident in EXPECTED:
                reading = api('/api/peripherals/read?id='+ident)
                row['readings'][ident] = reading
                try: reading['acceptance'] = validate_reading(ident, configs[ident], reading,
                                                            args.require_current_diagnostics)
                except ValueError as error: report['errors'].append(dict(index=index,error=str(error)))
            uptime = api('/api/system/info')['uptime']['seconds']
            if not isinstance(uptime, (int,float)) or uptime < previous:
                raise ValueError('Device restarted or uptime is invalid')
            row['uptime'] = uptime; previous = uptime; report['samples'].append(row)
            print(json.dumps(row),flush=True)
            if index+1 < args.samples: time.sleep(args.interval)
        report['finalUptime'] = previous
        temperatures = [r['readings']['ai_soil_temp'].get('value') for r in report['samples']]
        temperatures = [v for v in temperatures if type(v) in (int,float) and math.isfinite(v)]
        if temperatures:
            report['temperature'] = dict(min=min(temperatures),max=max(temperatures),mean=statistics.mean(temperatures))
            if args.reference_temperature is not None:
                report['temperature'].update(reference=args.reference_temperature,
                    differenceFromOperatorReference=statistics.mean(temperatures)-args.reference_temperature,
                    referenceIsTraceableCalibration=False)
        report['status'] = 'PASS' if not report['errors'] else 'FAIL'
    except Exception as error:
        report['errors'].append(dict(error=str(error)))
    out.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
    print(json.dumps(dict(status=report['status'],report=str(out),errors=report['errors'])),flush=True)
    return 0 if report['status']=='PASS' else 2

if __name__=='__main__': raise SystemExit(main())
