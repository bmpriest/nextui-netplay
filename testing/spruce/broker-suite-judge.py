#!/usr/bin/env python3
"""Archive a full gameplay session and gate measured shim connection coverage."""
from datetime import datetime
import json
from pathlib import Path
import re
import shlex
import shutil
import sys

from judge import inspect


def main(attempt):
    attempt = Path(attempt)
    rendered = list(attempt.glob('rendered-*.test'))
    if len(rendered) != 1:
        return {'verdict': 'INFRA_FAIL', 'reason': 'missing exact rendered gameplay script'}
    session = shlex.split(rendered[0].read_text().splitlines()[0])
    source = Path(session[2]) / session[1]
    destination = attempt / 'gameplay'
    shutil.copytree(source, destination, dirs_exist_ok=True)
    events = [json.loads(line) for line in (destination / 'events.jsonl').read_text().splitlines()]
    starts = []
    for event in events:
        line = event.get('line', '')
        match = re.search(r'gameplay\.sh --bg (\d+)', line)
        if event.get('event') == 'command' and match:
            starts.append((datetime.fromisoformat(event['time']).timestamp(), int(match[1])))
    if not starts:
        # Advance Wars measures a verified turn exchange rather than random play.
        day1 = [datetime.fromisoformat(e['time']).timestamp() for e in events
                if e.get('event') == 'verify' and e.get('matched') and 'aw2-day1-' in e.get('reference', '')]
        day2 = [datetime.fromisoformat(e['time']).timestamp() for e in events
                if e.get('event') == 'verify' and e.get('matched') and 'aw2-day2-' in e.get('reference', '')]
        if len(day1) != 2 or len(day2) != 2:
            return {'verdict': 'INFRA_FAIL', 'reason': 'no complete named gameplay or two-turn interval'}
        start, end = max(day1), max(day2)
    else:
        start = max(t for t, _ in starts)
        end = min(t + duration for t, duration in starts)
    devices = {}
    for device in ('b', 'h'):
        log = attempt / 'measurement' / (device + '.log')
        if not log.exists():
            devices[device] = {'verdict': 'INFRA_FAIL', 'reason': 'missing device log'}
        else:
            devices[device] = inspect(log.read_text(errors='replace'), start, end)
    values = [row['verdict'] for row in devices.values()]
    verdict = 'PRODUCT_FAIL' if 'PRODUCT_FAIL' in values else ('INFRA_FAIL' if 'INFRA_FAIL' in values else 'PASS')
    return {'verdict': verdict, 'devices': devices, 'measurement': {'name': 'scripted_gameplay', 'start': start, 'end': end},
            'performance': 'UNVALIDATED: instrumentation overhead gate outstanding',
            'network_rtt': 'NO_DATA: no validated probe or session RTT adapter'}


if __name__ == '__main__':
    directory = Path(sys.argv[1])
    try:
        result = main(directory)
    except (OSError, ValueError, IndexError) as error:
        result = {'verdict': 'INFRA_FAIL', 'reason': str(error)}
    (directory / 'connection-verdict.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    sys.exit(0 if result['verdict'] == 'PASS' else 1)
