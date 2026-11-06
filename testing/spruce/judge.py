#!/usr/bin/env python3
"""Conservative offline connection gate for archived spruce runs.

This is an interim adapter for existing shim logs. It never converts a screen
script's successful exit into proof of multiplayer. Typed session events will
replace these version-specific log patterns.
"""
import argparse
from datetime import datetime, timedelta
import json
from pathlib import Path
import re


STAMP = re.compile(r'^\[(\d\d:\d\d:\d\d\.\d+)\] \[netplay-shim\] (.*)')
LOSS = re.compile(r'continued this game solo|host disconnected|Peer disconnected|Peer unavailable|Core cannot save state|recovery failed|RetroArch client left.*playing on alone|ra: frame .*crc [1-9][0-9]* bad of', re.I)
PROGRESS = re.compile(r'in sync at frame|paired consoles agree|(?:instanced )?pacing:|ra: frame')


def inspect(text, start, end):
    progress = []
    losses = []
    date = datetime.fromtimestamp(end)
    for line in text.splitlines():
        match = STAMP.match(line)
        if not match:
            continue
        clock = datetime.strptime(match[1], '%H:%M:%S.%f')
        stamp = date.replace(hour=clock.hour, minute=clock.minute, second=clock.second,
                             microsecond=clock.microsecond)
        if stamp.timestamp() > end + 3600:
            stamp -= timedelta(days=1)
        when = stamp.timestamp()
        # A failed startup followed by solo gameplay invalidates the run even
        # when the disconnect preceded the measurement window.
        if when <= end and LOSS.search(match[2]):
            losses.append({'time': when, 'message': match[2]})
        if start - 10 <= when <= end and PROGRESS.search(match[2]):
            progress.append(when)
    if losses:
        return {'verdict': 'PRODUCT_FAIL', 'reason': 'multiplayer connection or synchronization failed', 'events': losses}
    if not progress or min(progress) > start + 10 or max(progress) < end - 12:
        return {'verdict': 'INFRA_FAIL', 'reason': 'insufficient multiplayer progress coverage',
                'samples': len(progress)}
    coverage = sorted([start, end] + [t for t in progress if start <= t <= end])
    gap = max(b - a for a, b in zip(coverage, coverage[1:]))
    if gap > 25:
        return {'verdict': 'INFRA_FAIL', 'reason': 'gap in multiplayer progress coverage',
                'max_progress_gap_s': gap, 'samples': len(progress)}
    return {'verdict': 'PASS', 'samples': len(progress),
            'first_progress': min(progress), 'last_progress': max(progress),
            'max_progress_gap_s': gap}


def judge(directory):
    directory = Path(directory)
    env = dict(line.split('=', 1) for line in (directory / 'run.env').read_text().splitlines() if '=' in line)
    if 'end' not in env:
        return {'verdict': 'INFRA_FAIL', 'reason': 'missing measurement end'}
    end = float(env['end'])
    start = float(env.get('start', end - float(env.get('window', 93))))
    devices = {}
    for name in ('brick', 'h700'):
        path = directory / (name + '.log')
        devices[name] = inspect(path.read_text(errors='replace'), start, end) if path.exists() else {
            'verdict': 'INFRA_FAIL', 'reason': 'missing device log'}
    verdicts = [row['verdict'] for row in devices.values()]
    verdict = 'PRODUCT_FAIL' if 'PRODUCT_FAIL' in verdicts else ('INFRA_FAIL' if 'INFRA_FAIL' in verdicts else 'PASS')
    return {'verdict': verdict, 'adapter': 'shim-log-v1', 'devices': devices,
            'measurement': {'start': start, 'end': end}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    args = parser.parse_args()
    result = judge(args.directory)
    (Path(args.directory) / 'connection-verdict.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['verdict'] == 'PASS' else 1)
