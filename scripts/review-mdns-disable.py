"""Sanitized offline review of mDNS-only configuration deployment and boot logs.

Never connects to devices. No private configuration, SSID or tokens are printed.
Boot-only evidence is explicitly separate from continuous/full-business gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def serial_summary(text):
    metrics=[]
    for match in re.finditer(r'\[METRICS\] heap=(\d+) min=(\d+) dram8=(\d+) blk8=(\d+) frag=(\d+)% mqtt_q=(\d+) sse=(\d+) poll=(\d+)ms up=(\d+)s',text):
        metrics.append(dict(zip(['heap','historicalMinHeap','dram8','blk8','fragmentationPercent',
            'mqttQueue','sseClients','pollMs','uptimeSeconds'],map(int,match.groups()))))
    complete=[]; current=None; active=None
    for line in text.splitlines():
        if '[PeriphExec] Executing action:' in line:
            active=None
            if 'type=19 on dht11,' in line:
                field=re.search(r'"dataField"\s*:\s*"(temperature|humidity)"',line)
                if field:
                    active=field[1]
                    if active=='temperature':
                        if current is not None: complete.append(current)
                        current=dict(readings={},results={})
        if current is None or active is None: continue
        reading=re.search(r'Sensor read: periph=dht11 cat=dht11 raw=(-?\d+(?:\.\d+)?) .*label=(temperature|humidity)',line)
        if reading: current['readings'][reading[2]]=float(reading[1])
        result=re.search(r'\[PeriphExec\] Action completed:.*result=(SUCCESS|FAILED)',line)
        if result:
            current['results'][active]=result[1]; active=None
    if current is not None: complete.append(current)
    paired=[x for x in complete if set(x['results'])=={'temperature','humidity'}]
    successful=[x for x in paired if set(x['readings'])=={'temperature','humidity'} and all(v=='SUCCESS' for v in x['results'].values())]
    return dict(metricCount=len(metrics),metrics=metrics,
      minimumCurrentDram8=min((x['dram8'] for x in metrics),default=None),
      minimumCurrentLargestBlock=min((x['blk8'] for x in metrics),default=None),
      historicalMinimumSinceBoot=min((x['historicalMinHeap'] for x in metrics),default=None),
      metricUptimeRegressions=sum(b['uptimeSeconds']<=a['uptimeSeconds'] for a,b in zip(metrics,metrics[1:])),
      configDisabledLogCount=len(re.findall(r'Config loaded - enableMDNS=false',text)),
      mdnsStartLogCount=len(re.findall(r'DNSManager: (?:Starting mDNS|mDNS started:)',text)),
      panicLogCount=len(re.findall(r'Guru Meditation|Core\s+\d+\s+panic|Backtrace:|assert failed:',text,re.I)),
      physicalDhtFailureCount=len(re.findall(r'\[SensorDriver\] DHT (?:read failed|bus lock timeout)',text)),
      completeNaturalDhtCycles=len(paired),successfulNaturalDhtCycles=len(successful),
      incompleteNaturalDhtCycles=len(complete)-len(paired),
      webServerStarted=bool(re.search(r'WebServer.*(?:started|Started|启动)|HTTP.*(?:server.*started|Server.*started)',text)))


def review(directory):
    path=Path(directory)
    report=json.loads((path/'deployment.json').read_text(encoding='utf-8-sig'))
    raw=(path/'post-disable-serial.log').read_bytes()
    summary=serial_summary(raw.decode('utf-8',errors='replace'))
    summary.update(port=report['port'],deploymentVerified=report.get('verified') is True,
        filesystemWritten=report.get('filesystemWritten'),applicationWritten=report.get('applicationWritten'),
        portClosed=report.get('portClosed'),serialSha256=hashlib.sha256(raw).hexdigest(),
        capture=report.get('postResetCapture'),firmwareSha256=report.get('firmwareSha256'),
        filesystemSha256=report.get('filesystemSha256'),longTermAcceptance='IN_PROGRESS',
        overallAcceptance='NOT_PASSED',networkTroubleshooting='EXCLUDED_BY_USER',
        limitations=['Boot capture only; not 24h continuity, physical cold-power or full business proof',
            'Different uptime/network phases prevent assigning all heap differences to mDNS',
            'Historical low-water mark is not current free memory'])
    summary['configurationOffVerified']=(summary['deploymentVerified'] and
        summary['configDisabledLogCount']>0 and summary['mdnsStartLogCount']==0)
    limits=json.loads((Path(__file__).resolve().parents[1]/'scripts/device-stability-thresholds.json').read_text(encoding='utf-8'))['presets']['release']['thresholds']
    summary['shortWindowMemoryGate']=('UNOBSERVED' if not summary['metrics'] else
        'PASS' if summary['minimumCurrentDram8']>=limits['minHeapFreeBytes'] and summary['minimumCurrentLargestBlock']>=limits['minHeapMaxAllocBytes']
        and not summary['panicLogCount'] and not summary['metricUptimeRegressions'] else 'FAIL')
    return summary


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args(); result=review(args.directory)
    if args.output:
        with args.output.open('x',encoding='utf-8') as stream: json.dump(result,stream,indent=2)
    print(json.dumps(result,ensure_ascii=True,indent=2))
