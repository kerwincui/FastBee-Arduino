"""Audit the control-selected observation without HTTP, serial or device writes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
CONTROL=ROOT/'artifacts/com4-com16-20260929/repair-control-20260930.json'


def read(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def dt(value): return datetime.fromisoformat(value.replace('Z','+00:00'))
def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_rows(path):
    lines=path.read_text(encoding='utf-8').splitlines(); rows=[]
    for i,line in enumerate(lines):
        try: rows.append(json.loads(line))
        except json.JSONDecodeError:
            if i!=len(lines)-1: raise
    return rows


def audit(control_path=CONTROL):
    control=read(control_path); directory=Path(control['activeMonitorDirectory'])
    run=read(directory/'run.json'); summary=read(directory/'summary.json')
    now=datetime.now(timezone.utc)
    script=Path(run['script'])
    ps="@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^python(w)?[.]exe$' -and $_.CommandLine -match 'monitor-(devices|after-r[23456]-20260930)[.]py' } | ForEach-Object { @{pid=$_.ProcessId;parentPid=$_.ParentProcessId;image=$_.ExecutablePath;createdAt=$_.CreationDate.ToString('o');commandLine=$_.CommandLine} }) | ConvertTo-Json -Compress"
    output=subprocess.check_output(['powershell.exe','-NoProfile','-Command',ps],text=True).strip()
    processes=json.loads(output) if output else []
    if isinstance(processes,dict): processes=[processes]
    live=next((p for p in processes if p['pid']==run['pid']),None)
    launchers=[p for p in processes if live and p['pid']==live['parentPid'] and
               p['commandLine'].split(' -u ',1)[-1]==live['commandLine'].split(' -u ',1)[-1] and
               '/penv/scripts/python.exe' in p['image'].replace('\\','/').lower()]
    identity=bool(live and control['activeMonitorPid']==run['pid'] and
      dt(live['createdAt'])==dt(control['activeMonitorCreatedAt']) and
      live['commandLine']==control['activeMonitorCommandLine'])
    hashes=(digest(script)==run['scriptSha256'] and
            digest(ROOT/'scripts/device-stability-thresholds.json')==run['thresholdsSha256'])
    age=(now-dt(summary['updatedAt'])).total_seconds()
    report=dict(checkedAt=now.isoformat(),runDirectory=str(directory),summaryCutoff=summary['updatedAt'],
        processIdentityMatches=identity,actualSamplerCount=len(processes)-len(launchers),sourceHashesMatch=hashes,
        summaryAgeSeconds=round(age,3),freshNonemptySummary=age<=180 and summary['cycles']>0,
        monitorPid=run['pid'],first24hStatus=summary['first24hStatus'],overallAcceptance='NOT_PASSED',
        memorySoftwareAcceptance='IN_PROGRESS',networkTroubleshooting='EXCLUDED_BY_USER',devices={},
        historyPreserved=True,physicalBusinessAcceptance='NOT_TESTED')
    raw=[row for path in sorted(directory.glob('samples-*.jsonl')) for row in read_rows(path)]
    rows=[row for row in raw if dt(row['finishedAt'])<=dt(summary['updatedAt'])]
    limits=read(ROOT/'scripts/device-stability-thresholds.json')['presets']['release']['thresholds']
    for port,stat in summary['devices'].items():
        samples=[row for row in rows if row['port']==port]
        complete=[row for row in samples if row.get('complete')]
        requests=[request for row in samples for request in row['requests']]
        failures=[r for r in requests if r.get('errorType') or r.get('httpStatus',200)!=200]
        latencies=sorted(r['elapsedMs'] for r in requests)
        gaps=[(dt(b['at'])-dt(a['at'])).total_seconds() for a,b in zip(samples,samples[1:])]
        keys=('attempts','completeSamples','healthySamples','requests','requestFailures','issueCounts',
              'http429Count','uptimeResets','stagnantUptimeSamples','samplingGaps','minimumDram8Free',
              'minimumLargestBlock','latest','first24hStatus')
        row={key:stat.get(key) for key in keys}
        span=(dt(complete[-1]['finishedAt'])-dt(complete[0]['finishedAt'])).total_seconds() if len(complete)>1 else 0
        row.update(rawCountsMatch=(len(samples)==stat['attempts'] and len(complete)==stat['completeSamples']
                  and len(requests)==stat['requests'] and len(failures)==stat['requestFailures']),
            actualCompleteSpanSeconds=span,completeCoveragePercent=100*len(complete)/len(samples) if samples else 0,
            requestFailurePercent=100*len(failures)/len(requests) if requests else None,
            requestStartGapsOver180=sum(g>180 for g in gaps),
            rawP95Ms=latencies[math.ceil(.95*len(latencies))-1] if latencies else None,
            latestCompleteAgeSeconds=(now-dt(complete[-1]['finishedAt'])).total_seconds() if complete else None)
        violation=any(s['snapshot'].get('dram8Free',0)<limits['minHeapFreeBytes'] or
                      s['snapshot'].get('dram8Largest',0)<limits['minHeapMaxAllocBytes'] for s in complete)
        row['memoryEvidenceStatus']='FAIL' if violation else 'UNOBSERVED' if not complete else 'IN_PROGRESS'
        row['memory24hStatus']='FAIL' if violation else 'IN_PROGRESS'
        # This audit never upgrades acceptance based on time alone or joins windows.
        report['devices'][port]=row
    for key,default in [('stdout','monitorStdout'),('stderr','monitorStderr')]:
        filename=control.get('activeMonitor'+key.capitalize()) or control.get(default)
        if filename:
            path=Path(filename); report[key+'Bytes']=path.stat().st_size
            # Log counts are sufficient for routine audit; do not leak raw private text.
    report['eventTail']=[{k:r.get(k) for k in ['at','event','port','issues']} for r in read_rows(directory/'events.jsonl')[-6:]]
    report['validObserver']=(identity and hashes and age<=180 and report['actualSamplerCount']==1
       and all(r['rawCountsMatch'] for r in report['devices'].values()))
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args(); result=audit()
    if args.output:
        with args.output.open('x',encoding='utf-8') as stream: json.dump(result,stream,indent=2)
    print(json.dumps(result,ensure_ascii=True,indent=2))
