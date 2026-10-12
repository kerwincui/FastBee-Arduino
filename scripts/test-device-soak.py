"""Long-duration read-only COM15 acceptance with incremental evidence.

No serial access, relay switching, configuration changes, or automatic resets.
Credentials remain in process environment. A RUNNING result is never a pass.
"""
import argparse
import hashlib
from datetime import datetime, timezone, timedelta
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import sys
import threading
import time
from requests.exceptions import ChunkedEncodingError, ContentDecodingError

STABILITY_COUNTERS = ['transportErrors', 'httpErrorCount', 'http429Count', 'restarts',
                      'invalidModbusSamples', 'relayReadErrors', 'onboardRelayReadErrors', 'digitalReadErrors',
                      'pollCounterResets', 'pollStalls', 'severeMemorySamples', 'degradedMemorySamples', 'memoryMetricErrors', 'samplingGaps',
                      'deploymentChanges', 'configChanges']
DAY_SECONDS = 86400
MAX_SOAK_SECONDS = 7 * DAY_SECONDS
REPORT_REPLACE_DELAYS = (.025, .05, .075)
# Separate from the sampling clock: virtual soak tests patch time.sleep.
_report_publish_wait = threading.Event().wait


def save_report_atomic(output, report):
    """Retry only Windows sharing/access contention, keeping atomic replacement."""
    pending, target = output / 'report.pending', output / 'report.json'
    report.setdefault('reportPublishRetries', 0)
    report.setdefault('reportPublishFailures', 0)
    counts = report.setdefault('reportPublishErrorCounts', {})
    def write_pending():
        pending.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
    write_pending()
    for attempt in range(len(REPORT_REPLACE_DELAYS) + 1):
        try:
            pending.replace(target)
            return
        except OSError as error:
            winerror = getattr(error, 'winerror', None)
            kind = ('WINERROR_' + str(winerror) if sys.platform == 'win32' and winerror is not None
                    else type(error).__name__)
            counts[kind] = counts.get(kind, 0) + 1
            retry = (sys.platform == 'win32' and winerror in (5, 32, 33) and
                     attempt < len(REPORT_REPLACE_DELAYS))
            if retry:
                report['reportPublishRetries'] += 1
            else:
                report['reportPublishFailures'] += 1
                report['status'] = 'FAIL'
            # Preserve the final failure in pending even when target stays locked.
            write_pending()
            if not retry:
                raise
            _report_publish_wait(REPORT_REPLACE_DELAYS[attempt])


def validate_timing(seconds, interval):
    if (type(seconds) is not int or not 30 <= seconds <= MAX_SOAK_SECONDS or
            type(interval) is not int or not 10 <= interval <= 300):
        raise ValueError('Invalid soak bounds: 30..604800 seconds, 10..300 interval')


def continuous_window_status(report, threshold):
    if report['requestedSeconds'] < threshold:
        return 'NOT_RUN'
    if report['status'] == 'RUNNING':
        return 'RUNNING'
    if (report.get('stabilityStatus') == 'FAIL' or report.get('measurementStatus') == 'FAIL' or
            report['status'] == 'FAIL'):
        return 'FAIL'
    if (report['status'] == 'PASS' and report.get('coveragePassed') and
            report.get('actualSampleSpanSeconds', 0) >= threshold):
        return 'PASS'
    return 'INCOMPLETE'


class ObservationChanged(RuntimeError):
    def __init__(self, counter, message):
        super().__init__(message)
        self.counter = counter


class DeploymentBinding:
    """Pin the existing deployment evidence without exposing private config."""
    def __init__(self, filename):
        self.path = Path(filename)
        contents = self.path.read_bytes()
        self.digest = hashlib.sha256(contents).hexdigest()
        self.deployment = json.loads(contents)
        if not isinstance(self.deployment, dict):
            raise ValueError('Deployment report must be an object')
        if (any(self.deployment.get(key) is not True for key in
                ('written', 'verified', 'applicationWritten')) or
                re.fullmatch('[0-9a-fA-F]{64}', str(self.deployment.get('firmwareSha256', ''))) is None):
            raise ValueError('Verified application deployment with firmware SHA256 required')

    def check(self):
        try:
            unchanged = hashlib.sha256(self.path.read_bytes()).hexdigest() == self.digest
        except OSError:
            unchanged = False
        if not unchanged:
            raise ObservationChanged('deploymentChanges', 'Deployment report changed or unavailable during observation')


def config_digest(configs, master):
    return hashlib.sha256(json.dumps(dict(peripherals=configs, master=master),
        sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf8')).hexdigest()


class MemoryMetricError(ValueError):
    """Successful HTTP response lacked valid internal-memory measurements."""


def internal_dram_sample(memory):
    """Require measured internal DRAM; total heap/PSRAM is not a substitute."""
    if not isinstance(memory, dict):
        raise MemoryMetricError('Missing internal DRAM metrics')
    result = {}
    for field in ('dram8Free', 'dram8Largest'):
        value = memory.get(field)
        if type(value) is not int or value < 0:
            raise MemoryMetricError('Missing or invalid internal DRAM metrics: ' + field)
        result[field] = value
    return result

def minimum_samples(seconds, interval):
    return max(2, math.ceil(seconds / interval * .9))


class CoverageTracker:
    """Measure gaps between completed samples, including startup and trailing gaps."""
    def __init__(self, report, started, interval):
        self.report, self.started = report, started
        self.limit = max(90, interval * 3)
        self.last_success = self.previous_attempt = None
        self.first_success = None
        self.gap_open = False
        report.update(durationComplete=False, coveragePassed=False,
                      lastSuccessfulSampleAt=None, measurementUnavailableSamples=0, actualSampleSpanSeconds=0)

    def update(self, now):
        r = self.report
        elapsed = max(0, now - self.started)
        gap = max(0, now - (self.last_success if self.last_success is not None else self.started))
        r['observedSeconds'] = elapsed
        r['lastSuccessfulSampleAgeSeconds'] = gap if self.last_success is not None else None
        r['timeWithoutSuccessfulSampleSeconds'] = gap
        r['maximumSamplingGapSeconds'] = max(r.get('maximumSamplingGapSeconds', 0), gap)
        if gap > self.limit and not self.gap_open:
            r['samplingGaps'] = r.get('samplingGaps', 0) + 1
            self.gap_open = True
        r['durationComplete'] = elapsed >= r['requestedSeconds']
        r['coveragePassed'] = bool(r['durationComplete'] and
            r['actualSampleSpanSeconds'] >= r['requestedSeconds'] and
            r['samples'] >= r['minimumRequiredSamples'] and not r.get('samplingGaps'))
        if r.get('samplingGaps'):
            r['stabilityStatus'] = 'FAIL'

    def attempt(self, now):
        if self.previous_attempt is not None:
            gap = now - self.previous_attempt
            self.report['maximumAttemptGapSeconds'] = max(
                self.report.get('maximumAttemptGapSeconds', 0), gap)
        self.previous_attempt = now
        self.report['attempts'] += 1
        self.update(now)

    def complete(self, now, at, complete):
        self.update(now)
        if complete:
            self.report['samples'] += 1
            if self.first_success is None:
                self.first_success = now
                self.report['firstSuccessfulSampleAt'] = at
            self.report['actualSampleSpanSeconds'] = max(0, now - self.first_success)
            self.last_success = now
            self.report['lastSuccessfulSampleAt'] = at
            self.gap_open = False
        else:
            self.report['measurementUnavailableSamples'] += 1
            self.report['measurementStatus'] = 'FAIL'
        self.update(now)


def classify_error(error):
    # requests may attach a 200 response to a body failure. Keep the failure
    # type instead of misclassifying a truncated/gzip-invalid body as HTTP_200.
    if isinstance(error, ChunkedEncodingError):
        return 'ChunkedEncodingError'
    if isinstance(error, ContentDecodingError):
        return 'ContentDecodingError'
    text = str(error)
    response = getattr(error, 'response', None)
    status = getattr(response, 'status_code', None)
    if status is not None:
        return 'HTTP_' + str(status)
    for token, kind in [('WinError 10051', 'NETWORK_UNREACHABLE'),
                        ('WinError 10065', 'HOST_UNREACHABLE'),
                        ('ConnectTimeout', 'CONNECT_TIMEOUT'),
                        ('ReadTimeout', 'READ_TIMEOUT'),
                        ('WinError 10054', 'CONNECTION_RESET')]:
        if token in text or token in type(error).__name__:
            return kind
    return type(error).__name__


def record_error(report, kind, error, at):
    event = dict(at=at, kind=kind, errorType=classify_error(error), error=str(error))
    report['latestError'] = event
    report.setdefault('lastErrorByKind', {})[kind] = event
    counts = report.setdefault('errorTypeCounts', {})
    counts[event['errorType']] = counts.get(event['errorType'], 0) + 1
    examples = report.setdefault('errorExamples', [])
    if len(examples) < 30 and (kind != 'soil' or sum(e['kind'] == 'soil' for e in examples) < 3):
        examples.append(event)
    return event


def finish_report(report, completed=False, interrupted=False):
    failed = (report.get('stabilityStatus') == 'FAIL' or report.get('reportPublishFailures', 0) or
              any(report.get(k, 0) for k in STABILITY_COUNTERS))
    measured_failed = report.get('measurementStatus') == 'FAIL' or any(
        report.get(k, 0) for k in ['invalidSoilSamples', 'invalidModbusSamples', 'measurementUnavailableSamples'])
    covered = report.get('durationComplete') and report.get('coveragePassed')
    stable = bool(completed and covered and report.get('pollsAdvanced') and not failed)
    report['stabilityStatus'] = 'PASS' if stable else ('FAIL' if failed or completed else 'INCOMPLETE')
    report['measurementStatus'] = ('FAIL' if measured_failed else
        ('PASS' if completed and covered and report.get('samples') else 'INCOMPLETE'))
    report['status'] = ('INTERRUPTED' if interrupted else
        ('PASS' if stable and report['measurementStatus'] == 'PASS' else 'FAIL'))
    report['continuous24hStatus'] = continuous_window_status(report, DAY_SECONDS)
    report['continuous7dStatus'] = continuous_window_status(report, MAX_SOAK_SECONDS)


def validate_onboard_config(config, relay_id, gpio):
    params = config.get('params', {})
    if (config.get('id') != relay_id or config.get('type') != 12 or
            config.get('enabled') is not True or config.get('pins') != [gpio] or
            type(params.get('initialState')) is not int or params['initialState'] != 1 or
            params.get('invertLogic', False) is not False):
        raise ValueError('Onboard NC lamp GPIO/startup configuration differs from accepted HIGH')

def onboard_relay_reading(reading, rule, relay_id, rule_id):
    state = reading.get('state')
    if type(state) is not int or state not in (0, 1):
        raise ValueError('Missing or invalid onboard GPIO state')
    actions = rule.get('actions', [])
    triggers = rule.get('triggers', [])
    if (rule.get('id') != rule_id or rule.get('enabled') is not True or
            len(actions) != 1 or actions[0].get('targetPeriphId') != relay_id or
            actions[0].get('actionType') != 30 or len(triggers) != 1):
        raise ValueError('Onboard photoelectric toggle rule changed or disabled')
    trigger = triggers[0]
    if (trigger.get('triggerType') != 4 or trigger.get('triggerPeriphId') != 'di1' or
            trigger.get('eventId') != 'ds:di1' or trigger.get('compareValue') != '0'):
        raise ValueError('Onboard photoelectric trigger changed')
    for key in ('triggerCount', 'lastTriggerTime'):
        if type(trigger.get(key)) is not int or trigger[key] < 0:
            raise ValueError('Missing or invalid photoelectric trigger counter')
    return dict(state=state, ruleId=rule_id, triggerCount=trigger['triggerCount'],
                lastTriggerTime=trigger['lastTriggerTime'],
                evidence='firmware state only; physical contact and lamp not observed')

def read_api(session, base, path, params, report):
    # Preserve every HTTP failure; reauthentication would hide expiry and evict observers.
    started = time.monotonic()
    response = None
    failure = None
    try:
        response = session.get(base + path, params=params, timeout=(3, 12))
        record_http_failure(response, report)
        response.raise_for_status()
        body = response.json()
        if not isinstance(body, dict) or body.get('success') is not True:
            raise ValueError('API failed: ' + path)
        return body.get('data') or {}
    except Exception as error:
        failure = error
        raise
    finally:
        report['lastRequestDiagnostic'] = request_diagnostic(path, started, response, failure)


def request_diagnostic(path, started, response=None, error=None):
    # Header values, cookies, tokens, credentials and query strings are never
    # persisted. Presence flags distinguish a dropped cookie from a rejected
    # session on the next authorized run; they make no device requests.
    from collections.abc import Mapping
    if response is None:
        response = getattr(error, 'response', None)
    request = getattr(response, 'request', None)
    if request is None:
        request = getattr(error, 'request', None)
    headers = getattr(request, 'headers', None)
    response_headers = getattr(response, 'headers', None)
    def present(values, name):
        if not isinstance(values, Mapping):
            return None
        return any(str(key).lower() == name and bool(value) for key, value in values.items())
    status = getattr(response, 'status_code', None)
    if type(status) is not int:
        status = None
    kind = classify_error(error) if error is not None else None
    phase = ('connect' if kind == 'CONNECT_TIMEOUT' else
             'response_read' if kind == 'READ_TIMEOUT' else
             'response_body' if kind in ('ChunkedEncodingError', 'ContentDecodingError') else
             'http_response' if status is not None else 'unknown')
    return dict(path=path.split('?', 1)[0],
                elapsedMs=round((time.monotonic() - started) * 1000, 3),
                httpStatus=status, failurePhase=phase if error is not None else None,
                errorType=kind, cookieHeaderPresent=present(headers, 'cookie'),
                authorizationHeaderPresent=present(headers, 'authorization'),
                setCookieReceived=present(response_headers, 'set-cookie'))


def record_http_failure(response, report):
    if response.status_code >= 400:
        report['httpErrorCount'] += 1
        key = str(response.status_code)
        report['httpStatusCounts'][key] = report['httpStatusCounts'].get(key, 0) + 1
        if response.status_code == 429:
            report['http429Count'] += 1

def poll_health(previous, current, unchanged_seconds, maximum_interval):
    if type(current) is not int or current < 0:
        raise ValueError('Missing or invalid Modbus poll counter')
    reset = previous is not None and current < previous
    stalled = previous is not None and current == previous and unchanged_seconds > maximum_interval + 15
    return reset, stalled

def load_module(name, filename):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(filename))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
soil=load_module('soil_soak','test-soil-adc-device.py')
modbus=load_module('modbus_soak','test-modbus-device.py')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',required=True)
    parser.add_argument('--expected-mac',required=True)
    parser.add_argument('--seconds',type=int,default=28800)
    parser.add_argument('--interval',type=int,default=30)
    parser.add_argument('--out',required=True)
    parser.add_argument('--deployment',help='Existing verified application deployment report; required for >=24h')
    parser.add_argument('--onboard-relay-id')
    parser.add_argument('--onboard-rule-id')
    parser.add_argument('--onboard-gpio',type=int,default=15)
    args=parser.parse_args()
    if bool(args.onboard_relay_id) != bool(args.onboard_rule_id):
        raise ValueError('Specify both onboard relay and photoelectric rule identifiers')
    validate_timing(args.seconds,args.interval)
    if args.seconds >= DAY_SECONDS and not args.deployment:
        raise ValueError('A fixed --deployment report is required for 24h/7d observation')
    binding=DeploymentBinding(args.deployment) if args.deployment else None
    if not 0<=args.onboard_gpio<=48:raise ValueError('Invalid onboard GPIO')
    out=Path(args.out);out.mkdir(parents=True,exist_ok=False)
    now=datetime.now(timezone.utc)
    report=dict(status='RUNNING',startedAt=now.isoformat(),plannedEndAt=(now+timedelta(seconds=args.seconds)).isoformat(),
                requestedSeconds=args.seconds,intervalSeconds=args.interval,pid=os.getpid(),samples=0,
                stabilityStatus='RUNNING',measurementStatus='RUNNING',transportErrors=0,http429Count=0,
                httpErrorCount=0,httpStatusCounts={},
                restarts=0,invalidSoilSamples=0,invalidSoilById={},invalidModbusSamples=0,relayReadErrors=0,
                onboardRelayReadErrors=0,digitalReadErrors=0,pollCounterResets=0,pollStalls=0,severeMemorySamples=0,degradedMemorySamples=0,memoryMetricErrors=0,samplingGaps=0,
                deploymentChanges=0,configChanges=0,
                minimumRequiredSamples=minimum_samples(args.seconds,args.interval),attempts=0,errorExamples=[],
                unverified=['traceable-sensor-accuracy','physical-edge-latency'])
    report.update(schemaVersion=3,observedSeconds=0,durationComplete=False,coveragePassed=False,measurementUnavailableSamples=0,
                  acceptanceStatus='INCOMPLETE',observerSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  expectedMac=args.expected_mac,continuous24hStatus='RUNNING' if args.seconds>=DAY_SECONDS else 'NOT_RUN',
                  continuous7dStatus='RUNNING' if args.seconds>=MAX_SOAK_SECONDS else 'NOT_RUN')
    if binding:
        report.update(deploymentReportSha256=binding.digest,firmwareSha256=binding.deployment['firmwareSha256'],
                      webSha256=binding.deployment.get('webSha256'))
    started=time.monotonic();deadline=started+args.seconds;previous=None;first_polls=None;last_polls=None
    coverage=None;session=None
    def save():
        if coverage is not None:coverage.update(time.monotonic())
        report['updatedAt']=datetime.now(timezone.utc).isoformat()
        report['elapsedSeconds']=round(time.monotonic()-started,1)
        save_report_atomic(out, report)
    def example(kind,message):
        event=record_error(report,kind,message,datetime.now(timezone.utc).isoformat())
        with (out/'events.jsonl').open('a',encoding='utf8') as events:
            events.write(json.dumps(event,ensure_ascii=False)+'\n')
    save()
    try:
        import requests
        session=requests.Session();session.trust_env=False;base=args.url.rstrip('/')
        def login():
            r=session.post(base+'/api/auth/login',data=dict(username=os.environ.get('FASTBEE_USERNAME','admin'),
                password=os.environ['FASTBEE_PASSWORD']),timeout=(3,12))
            record_http_failure(r,report);r.raise_for_status()
            if r.json().get('success') is not True:raise ValueError('Authentication failed')
        def api(path,params=None):
            return read_api(session,base,path,params,report)
        login()
        norm=lambda value:re.sub('[^0-9a-f]','',value.lower())
        if len(norm(args.expected_mac))!=12:raise ValueError('Invalid expected MAC')
        identity=api('/api/system/info')
        if norm(identity.get('network',{}).get('macAddress',''))!=norm(args.expected_mac):raise ValueError('Device identity mismatch')
        if args.onboard_relay_id:
            onboard_config=api('/api/peripherals/',{'id':args.onboard_relay_id})
            validate_onboard_config(onboard_config,args.onboard_relay_id,args.onboard_gpio)
            report['onboardRelayConfig']=onboard_config
            report['onboardRuleId']=args.onboard_rule_id
        configs={ident:api('/api/peripherals/',{'id':ident}) for ident in soil.EXPECTED}
        for ident,config in configs.items():soil.validate_config(ident,config)
        report['soilConfigs']=configs
        master=api('/api/protocol/config',{'section':'modbusRtu'})['modbusRtu']['master']
        baseline_config_digest=config_digest(configs,master)
        report['configSha256']=baseline_config_digest
        def verify_bound_configuration():
            if not binding:return
            binding.check()
            current_configs={ident:api('/api/peripherals/',{'id':ident}) for ident in configs}
            current_master=api('/api/protocol/config',{'section':'modbusRtu'})['modbusRtu']['master']
            if config_digest(current_configs,current_master)!=baseline_config_digest:
                raise ObservationChanged('configChanges','Observed configuration changed; start a new window')
        devices=master['devices']
        relays=[d for d in devices if d.get('enabled') and d.get('deviceType')=='relay' and d.get('slaveAddress')==6]
        if len(relays)!=1:raise ValueError('Expected one address-6 relay')
        relay=relays[0]
        # Resolve identifiers from enabled configured sensors instead of a prior sample.
        expected_channels = {m['sensorId'] for d in master['tasks'] if d.get('enabled')
                             for m in d.get('mappings', []) if m.get('sensorId')}
        if len(expected_channels) != 7:raise ValueError('Expected seven configured Modbus measurement channels')
        report['expectedModbusChannels']=sorted(expected_channels)
        previous_polls=None;last_poll_progress=time.monotonic()
        relay_params=dict(slaveAddress=6,channelCount=relay['channelCount'],coilBase=relay.get('coilBase',0),
                          mode='register' if relay.get('controlProtocol')==1 else 'coil')
        observation_started=time.monotonic();deadline=observation_started+args.seconds
        report['observationStartedAt']=datetime.now(timezone.utc).isoformat()
        report['plannedEndAt']=(datetime.now(timezone.utc)+timedelta(seconds=args.seconds)).isoformat()
        coverage=CoverageTracker(report,observation_started,args.interval)
        with (out/'samples.jsonl').open('w',encoding='utf8',buffering=1) as stream:
            while True:
                tick=time.monotonic();row=dict(at=datetime.now(timezone.utc).isoformat(),readings={})
                coverage.attempt(tick);fatal=None;row['complete']=False
                failures_before=sum(report[k] for k in ['invalidModbusSamples','relayReadErrors','onboardRelayReadErrors','digitalReadErrors'])
                try:
                    if binding:
                        binding.check()
                        if (report['attempts']-1)%5==0:
                            verify_bound_configuration()
                    info=api('/api/system/info')
                    if norm(info.get('network',{}).get('macAddress',''))!=norm(args.expected_mac):
                        raise RuntimeError('Device identity changed; stopping all reads')
                    uptime=info.get('uptime',{}).get('seconds')
                    if type(uptime) not in (int,float) or not math.isfinite(uptime) or uptime<0:raise ValueError('Missing or invalid uptime')
                    if previous is not None and uptime<previous:report['restarts']+=1
                    previous=uptime;report.setdefault('initialUptime',uptime);report['finalUptime']=uptime
                    row['uptime']=uptime
                    health=api('/api/health')
                    row['memory']=health.get('memory',{})
                    row['memoryDegradation']=health.get('memoryDegradation',{})
                    # NORMAL is the current pressure level, not proof that all
                    # previously suspended services have actually recovered.
                    if (health.get('degraded') is True or row['memory'].get('degraded') is True or
                            row['memory'].get('critical') is True or row['memoryDegradation'].get('active') is True):
                        report['degradedMemorySamples']+=1
                        example('memoryDegradation',row['memoryDegradation'] or row['memory'])
                    system_status=api('/api/system/status')
                    if norm(system_status.get('macAddress',''))!=norm(args.expected_mac):
                        raise RuntimeError('Device identity changed during memory sampling')
                    row['internalMemory']=internal_dram_sample(system_status.get('memory',{}))
                    report['memoryMetricsSource']='/api/system/status:memory.dram8Free,dram8Largest'
                    for field,target in [('dram8Free','minimumDramFree'),('dram8Largest','minimumDramLargestBlock')]:
                        value=row['internalMemory'][field]
                        report[target]=min(report.get(target,value),value)
                    if row['memory'].get('guardLevel') in ('SEVERE','CRITICAL'):
                        report['severeMemorySamples']+=1;example('memory',row['memory'].get('guardLevel'))
                    status=api('/api/modbus/status',{'full':1})
                    row['modbus'],mapping_errors=modbus.mapped_readings(status)
                    row['modbusErrors']=mapping_errors
                    actual_channels={r['id'] for r in row['modbus']}
                    if mapping_errors or actual_channels!=expected_channels:
                        report['invalidModbusSamples']+=1
                        example('modbus',dict(errors=mapping_errors,missing=sorted(expected_channels-actual_channels)))
                    last_polls=status.get('totalPolls')
                    if first_polls is None:first_polls=last_polls
                    intervals=[t.get('pollInterval') for t in status.get('tasks',[]) if t.get('enabled')]
                    intervals=[v for v in intervals if type(v) in (int,float) and v>0]
                    if not intervals:raise ValueError('Missing Modbus polling intervals')
                    reset,stalled=poll_health(previous_polls,last_polls,time.monotonic()-last_poll_progress,max(intervals))
                    if reset:report['pollCounterResets']+=1;example('modbus','Polling counter decreased without a planned restart')
                    if stalled:report['pollStalls']+=1;example('modbus','Background polling stopped advancing')
                    if previous_polls is None or last_polls!=previous_polls:last_poll_progress=time.monotonic()
                    previous_polls=last_polls
                    row['totalPolls']=last_polls
                    for ident in soil.EXPECTED:
                        reading=api('/api/peripherals/read',{'id':ident});row['readings'][ident]=reading
                        try:soil.validate_reading(ident,configs[ident],reading)
                        except ValueError as error:
                            report['invalidSoilSamples']+=1
                            report['invalidSoilById'][ident]=report['invalidSoilById'].get(ident,0)+1
                            row.setdefault('soilErrors',{})[ident]=str(error);example('soil',error)
                    for ident in ['di1','di2']:
                        state=api('/api/peripherals/read',{'id':ident}).get('state');row[ident]=state
                        if state not in (0,1) or type(state) not in (bool,int):report['digitalReadErrors']+=1
                    if args.onboard_relay_id:
                        try:
                            row['onboardRelay']=onboard_relay_reading(
                                api('/api/peripherals/read',{'id':args.onboard_relay_id}),
                                api('/api/periph-exec',{'id':args.onboard_rule_id}),
                                args.onboard_relay_id,args.onboard_rule_id)
                        except (ValueError,KeyError,requests.RequestException) as error:
                            report['onboardRelayReadErrors']+=1;example('onboardRelay',error)
                    if (report['attempts']-1)%2==0:
                        try:row['relayStates']=modbus.validate_states(api('/api/modbus/coil/status',relay_params),relay['channelCount'])
                        except (ValueError,requests.RequestException) as error:report['relayReadErrors']+=1;example('relay',error)
                    row['complete']=sum(report[k] for k in ['invalidModbusSamples','relayReadErrors','onboardRelayReadErrors','digitalReadErrors'])==failures_before
                except (requests.RequestException,ValueError,KeyError,RuntimeError) as error:
                    counter=(error.counter if isinstance(error,ObservationChanged) else
                             'memoryMetricErrors' if isinstance(error,MemoryMetricError) else 'transportErrors')
                    report[counter]+=1;row['error']=str(error);example('sampling',error)
                    row['errorType']=classify_error(error)
                    if isinstance(error,RuntimeError):fatal=error
                except KeyboardInterrupt as error:
                    row['error']='Sampling interrupted';fatal=error;example('interrupted',row['error'])
                row['lastRequestDiagnostic']=report.get('lastRequestDiagnostic')
                row['completedAt']=datetime.now(timezone.utc).isoformat()
                coverage.complete(time.monotonic(),row['completedAt'],row['complete'])
                if row['complete'] and report['samples']==1:
                    # Initialization and the first HTTP batch do not count as a
                    # successful observation span. Never shorten the endpoint.
                    deadline=coverage.first_success+args.seconds
                    report['plannedEndAt']=(datetime.now(timezone.utc)+timedelta(seconds=args.seconds)).isoformat()
                if report['invalidSoilSamples'] or report['invalidModbusSamples']:
                    report['measurementStatus']='FAIL'
                if any(report[k] for k in STABILITY_COUNTERS):
                    report['stabilityStatus']='FAIL'
                stream.write(json.dumps(row,ensure_ascii=False)+'\n');save()
                if fatal is not None:raise fatal
                if tick>=deadline:break
                time.sleep(min(max(0,args.interval-(time.monotonic()-tick)),max(0,deadline-time.monotonic())))
        verify_bound_configuration()
        report['pollsAdvanced']=type(last_polls) is int and type(first_polls) is int and last_polls>first_polls
        coverage.update(time.monotonic());finish_report(report,completed=True)
        report['completedAt']=datetime.now(timezone.utc).isoformat()
    except Exception as error:
        finish_report(report)
        report['error']=str(error);example('fatal',error)
    except KeyboardInterrupt:
        finish_report(report,interrupted=True)
        report['interruptedAt']=datetime.now(timezone.utc).isoformat()
    finally:
        try:
            if session is not None:session.close()
        finally:save()
    print(json.dumps({k:v for k,v in report.items() if k!='soilConfigs'}),flush=True)
    return 0 if report['status']=='PASS' else 2

if __name__=='__main__':raise SystemExit(main())
