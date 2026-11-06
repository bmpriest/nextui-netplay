#!/usr/bin/env python3
"""Rebuild the protocol/game comparison from archived broker evidence only."""
import argparse
import csv
from collections import Counter
from datetime import datetime, timedelta
import json
from pathlib import Path
import re


def log_time(hms, end):
    day = datetime.fromtimestamp(end)
    h, m, s = hms.split(':')
    value = day.replace(hour=int(h), minute=int(m), second=int(float(s)), microsecond=round((float(s) % 1) * 1e6))
    if (value-day).total_seconds() > 43200:
        value -= timedelta(days=1)
    elif (day-value).total_seconds() > 43200:
        value += timedelta(days=1)
    return value.timestamp()


def rates(intervals, start, end):
    """Duration-weighted pacing; exact rolling 10s minimum on the logged bins.

    Uniform progress within each log bin is an estimate, not per-frame capture.
    Missing coverage is excluded, never treated as zero or as a freeze.
    """
    bins = [(max(a, start), min(b, end), f/(b-a)) for a,b,f in intervals
            if b > a and b > start and a < end]
    bins.sort()
    # Millisecond log timestamps can overlap adjacent duration bins slightly.
    disjoint = []
    for a,b,r in bins:
        if disjoint:
            a = max(a, disjoint[-1][1])
        if b > a:
            disjoint.append((a,b,r))
    bins = disjoint
    duration = sum(b-a for a,b,_ in bins)
    if not duration:
        return {'avg_fps': None, 'slowest_10s_fps': None, 'coverage_s': 0}
    slow = None
    candidates = {start, end-10}
    for a,b,_ in bins:
        candidates.update((a,b,a-10,b-10))
    for t in candidates:
        if t < start or t+10 > end:
            continue
        pieces = [(max(a,t),min(b,t+10),r) for a,b,r in bins if b>t and a<t+10]
        covered = sum(b-a for a,b,_ in pieces)
        if abs(covered-10) < .001:
            fps = sum((b-a)*r for a,b,r in pieces)/10
            slow = fps if slow is None else min(slow,fps)
    return {'avg_fps': sum((b-a)*r for a,b,r in bins)/duration,
            'slowest_10s_fps': slow, 'coverage_s': duration}


def device_metrics(text, protocol, start, end):
    intervals, previous, counters = [], None, []
    checks = failed = resends = 0
    logical_checks = {}
    raw_check_messages = 0
    check_observed = False
    freeze = None
    desync = None
    samples = 0
    for line in text.splitlines():
        match = re.match(r'\[(\d\d:\d\d:\d\d\.\d+)\] \[netplay-shim\] (.*)', line)
        if not match:
            continue
        t, msg = log_time(match[1],end), match[2]
        inside = start <= t <= end
        if protocol == 'retroarch':
            m = re.match(r'ra: frame (\d+), .*\(max (\d+)\).*crc (\d+) bad of (\d+), states (\d+) loaded',msg)
            if not m:
                continue
            frame, stall, bad, total, states = map(int,m.groups())
            if previous and frame >= previous[1]:
                intervals.append((previous[0],t,frame-previous[1]))
            previous = (t,frame)
            counters.append((t,bad,total,states))
            if inside:
                samples += 1
                freeze = max(freeze or 0,stall*1000/60)
        else:
            m = re.match(r'(?:(?:instanced|link) )?pacing: (\d+) (?:paired )?frames in (\d+)ms',msg)
            if m:
                frames, ms = map(int,m.groups())
                intervals.append((t-ms/1000,t,frames))
                if inside:
                    samples += 1
                    longest = re.search(r'longest (\d+),',msg)
                    if longest:
                        freeze = max(freeze or 0,int(longest[1])*1000/60)
            if not inside:
                continue
            if msg.startswith(('in sync at frame','paired consoles agree')):
                raw_check_messages += 1
                frame = re.search(r'at frame (\d+)',msg)
                if frame: logical_checks.setdefault(int(frame[1]),False)
                check_observed = True
            elif msg.startswith(('DESYNC','PAIRED DESYNC')):
                raw_check_messages += 1
                frame = re.search(r'at frame (\d+)',msg)
                if frame: logical_checks[int(frame[1])] = True
                desync = t; check_observed = True
            elif re.match(r'authoritative resync \d+ committed',msg):
                resends += 1
                if desync is not None:
                    freeze = max(freeze or 0,(t-desync)*1000); desync = None
    if protocol != 'retroarch':
        checks, failed = len(logical_checks),sum(logical_checks.values())
    if protocol == 'retroarch':
        before = [c for c in counters if c[0] <= start]
        within = [c for c in counters if start <= c[0] <= end]
        # Require a baseline; never include startup state transfers.
        if before and within:
            base,last = before[-1],within[-1]
            failed,checks,resends = (last[i]-base[i] for i in (1,2,3))
            check_observed = True
    result = rates(intervals,start,end)
    result.update(longest_freeze_ms=freeze, checks_failed=failed if check_observed else None,
                  checks_total=checks if check_observed else None,
                  state_resends=resends if check_observed else None, samples=samples, raw_check_messages=raw_check_messages)
    return result


def measurement_window(attempt):
    verdict = attempt/'connection-verdict.json'
    if verdict.exists():
        window = json.loads(verdict.read_text()).get('measurement')
        if window: return window
    # A failed post-play assertion skips judge, but the completed play window
    # remains usable evidence. Recover only explicitly logged gameplay commands.
    path = attempt/'gameplay/events.jsonl'
    if not path.exists(): return None
    events = [json.loads(line) for line in path.read_text().splitlines()]
    starts = []
    for event in events:
        match = re.search(r'gameplay\.sh --bg (\d+)',str(event.get('line','')))
        if event.get('event') == 'command' and match:
            starts.append((datetime.fromisoformat(event['time']).timestamp(),int(match[1])))
    if len(starts) >= 2:
        start,end = max(t for t,_ in starts), min(t+duration for t,duration in starts)
        # Both scripts must actually reach their end before failure/collection.
        last = max(datetime.fromisoformat(e['time']).timestamp() for e in events)
        if end > start and last >= end:
            return {'name':'scripted_gameplay','start':start,'end':end,'source':'archived gameplay events (judge skipped)'}
    return None


def measure(attempt, protocol):
    window = measurement_window(attempt)
    if not window:
        return {'status':'NO_DATA','reason':'No completed named gameplay interval; navigation failed or never reached measurement.'}
    start,end = window['start'],window['end']
    devices = {}
    for device in ('b','h'):
        path = attempt/'measurement'/f'{device}.log'
        if path.exists():
            devices[device] = device_metrics(path.read_text(errors='replace'),protocol,start,end)
    if not devices:
        return {'status':'NO_DATA','reason':'Missing both archived device logs','window':window}
    metrics = {}
    for key,fn in [('avg_fps',min),('slowest_10s_fps',min),('longest_freeze_ms',max)]:
        vals = [d.get(key) for d in devices.values()]
        metrics[key] = fn(vals) if len(vals)==2 and all(v is not None for v in vals) else None
    # Client checks/loads are authoritative; host normally reports zero CRCs.
    client = devices.get('h',{})
    for key in ('checks_failed','checks_total','state_resends'):
        metrics[key] = client.get(key)
    return {'status':'OBSERVED' if metrics['avg_fps'] is not None else 'NO_DATA',
            'window':window,'devices':devices,**metrics,
            'coverage_s':min(d['coverage_s'] for d in devices.values()), 'window_s':end-start,
            'sources':{d:f'measurement/{d}.log' for d in devices},
            'method':'Archived pacing bins; duration-weighted FPS and rolling 10s bin estimate. Slower device FPS, larger logged stall; guest checks and state loads. Stall frames converted at 60Hz. RA counters use last sample before start and last within window.'}


def fmt(value, suffix=''):
    return 'NO_DATA' if value is None else f'{value:.1f}{suffix}'


def write_report(root):
    root = Path(root)
    rows = json.loads((root/'results.json').read_text())
    suite = json.loads((root/'suite.json').read_text())['manifest']
    cells = {c['id']:c for c in suite['cells']}
    counts = Counter(r['verdict'] for r in rows)
    lines = [f"{len(rows)} planned cells: "+', '.join(f'{n} {v}' for v,n in sorted(counts.items()))+'.','',
             '| Protocol | Game / system / core or mode | Avg FPS | Slowest 10 s | Longest freeze | Checks failed | State resends | FPS coverage | Result | Evidence |',
             '|---|---|---:|---:|---:|---:|---:|---|---|---|']
    details = []
    csv_rows = []
    for row in sorted(rows,key=lambda r:(r['cell'].split('-')[0], list(cells).index(r['cell']))):
        cell = cells[row['cell']]
        args = cell['test']
        protocol, test, system, rom, core = args[3:8]
        label = 'LinkPlay' if protocol=='linkplay' else 'RetroArch over Netplay'
        game = re.sub(r'\.(zip|chd|nds)$','',rom,flags=re.I)
        game = re.sub(r' \([^)]*\)','',game)
        mode = core
        if core == 'default':
            mode = {'FC':'FCEUmm','MD':'PicoDrive','PS':'PCSX ReARMed',
                    'GB':'Gambatte paired','GBC':'Gambatte paired'}.get(system,'default core')
        if core == 'mednafen_supafaust': mode = 'Supafaust'
        if core == 'snes9x': mode = 'Snes9x'
        if core == 'cable': mode = 'Gambatte cable'
        if core == 'mgba': mode = 'mGBA'
        if core == 'gpsp': mode = 'gpSP'
        if system=='NDS':
            game = 'Mario Kart DS'
            mode = row['cell'].removeprefix('linkplay-nds-').replace('dsp','DSperate').replace('nds','DraStic')
        name = f'{game} / {system} / {mode}'
        artifact = row.get('artifacts')
        metric = measure(root/artifact,protocol) if artifact else {'status':'NO_DATA','reason':row.get('reason','No attempt')}
        row['telemetry'] = metric
        csv_rows.append({'protocol':label,'game':game,'system':system,'core_or_mode':mode,
                         'verdict':row['verdict'], **{key:metric.get(key) for key in
                         ('avg_fps','slowest_10s_fps','longest_freeze_ms','checks_failed','checks_total','state_resends','coverage_s','window_s')},
                         'evidence':artifact or ''})
        if artifact:
            (root/artifact/'telemetry.json').write_text(json.dumps(metric,indent=2)+'\n')
        checks = 'NO_DATA' if metric.get('checks_total') is None else f"{metric['checks_failed']} of {metric['checks_total']}"
        resends = metric.get('state_resends')
        resends = 'NO_DATA' if resends is None else str(resends)
        result = row['verdict']+(' (retry)' if row.get('passed_after_retry') else '')
        evidence = f'[logs + metrics]({artifact}/telemetry.json) · [verdict]({artifact}/result.json)' if artifact else 'Declared unsupported'
        coverage='NO_DATA' if 'window_s' not in metric else f"{metric['coverage_s']:.1f}/{metric['window_s']:.1f}s"
        lines.append(f"| {label} | {name} | {fmt(metric.get('avg_fps'))} | {fmt(metric.get('slowest_10s_fps'))} | {fmt(metric.get('longest_freeze_ms'),' ms')} | {checks} | {resends} | {coverage} | {result} | {evidence} |")
        if metric.get('window'):
            w=metric['window'];coverage=', '.join(f"{d}: {v['coverage_s']:.1f}s" for d,v in metric['devices'].items())
            details.append(f"- {row['cell']}: {w['end']-w['start']:.1f}s gameplay; logged FPS coverage {coverage}.")
        else:
            details.append(f"- {row['cell']}: {metric.get('reason','NO_DATA')}")
    lines += ['', 'FPS is reconstructed from existing log bins, with progress estimated uniformly inside each bin. Average and slowest 10 s use the slower device; longest freeze uses the larger reported stall, converted from stalled frames at 60 Hz. This is the logged network stall, not a video capture of every possible freeze. Checks failed count unique hash frames (redundant UDP hash deliveries are deduplicated) and state resends come from the guest; RetroArch startup state loads are excluded. Bin boundaries and cumulative counter samples limit measurement precision. NO_DATA means unavailable, never zero.', '',
              'These are observed run measurements. Instrumentation overhead has not been validated; the runs do not establish a controlled protocol performance comparison. No devices were rerun to generate this report.', '',
              'Gameplay windows and log coverage:', '', *details, '',
              'All earlier attempts and retries remain in results.json and the cell directories. The table uses each cell’s selected final attempt, including failures; no faster attempt is substituted.', '',
              '[Download comparison CSV](comparison.csv) · [Final baseline restoration](final-state/result.json).']
    with (root/'comparison.csv').open('w',newline='') as output:
        writer=csv.DictWriter(output,fieldnames=list(csv_rows[0]))
        writer.writeheader(); writer.writerows(csv_rows)
    (root/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    (root/'report.md').write_text('\n'.join(lines)+'\n')
    return lines


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path)
    args=parser.parse_args()
    write_report(args.run)
