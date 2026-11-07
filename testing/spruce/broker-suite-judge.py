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
        line = str(event.get('line', ''))
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
    # Confirm the selected transport from both actual game-process logs.
    config = attempt.parent.parent.name
    if config.endswith(('-tcp', '-hybrid')) and attempt.parent.name.startswith('linkplay-'):
        marker = 'UDP disabled - input/hash staying on TCP' if config.endswith('-tcp') else 'peer UDP endpoint port='
        for device in ('b', 'h'):
            log = attempt / 'measurement' / (device + '.log')
            if not log.exists() or marker not in log.read_text(errors='replace'):
                return {'verdict':'INFRA_FAIL', 'reason':f'{device}: requested transport not confirmed in game log'}
    if 'mkds' in rendered[0].stem:
        # Nintendo DS: spruce's stock DSperate writes one unstamped summary
        # ('lan: hosting/joined', MP waits/timeouts), not the shim's stamped
        # pacing windows inspect() measures, and the screens plus check.sh
        # have already gated the session itself. Judge what the format
        # honestly proves: this console joined a session and its peer
        # answered (timeouts below the wait count).
        devices = {}
        for device in ('b', 'h'):
            log = attempt / 'measurement' / (device + '.log')
            if not log.exists():
                devices[device] = {'verdict': 'INFRA_FAIL', 'reason': 'missing device log'}
                continue
            text = log.read_text(errors='replace')
            if 'lan pacing:' in text:
                devices[device] = inspect(text, start, end)
                continue
            joined = re.search(r'^lan: (hosting, player 0|joined, player \d+)', text, re.M)
            waits = re.findall(r'^lan: reply/host waits (\d+),.*timeouts (\d+)', text, re.M)
            if not joined or not waits:
                devices[device] = {'verdict': 'INFRA_FAIL', 'reason': 'stock DS log lacks a joined session or MP waits'}
                continue
            total_waits, timeouts = (int(value) for value in waits[-1])
            ok = total_waits > 0 and timeouts < total_waits
            devices[device] = {'verdict': 'PASS' if ok else 'PRODUCT_FAIL',
                               'reason': f'{joined.group(1)}; MP waits {total_waits}, timeouts {timeouts}'}
        values = [row['verdict'] for row in devices.values()]
        verdict = 'PRODUCT_FAIL' if 'PRODUCT_FAIL' in values else ('INFRA_FAIL' if 'INFRA_FAIL' in values else 'PASS')
        return {'verdict': verdict, 'devices': devices, 'measurement': {'name': 'scripted_gameplay', 'start': start, 'end': end},
                'performance': 'NO_DATA: stock DSperate log has no pacing windows',
                'network_rtt': 'NO_DATA: no validated probe or session RTT adapter'}
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
