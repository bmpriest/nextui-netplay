#!/usr/bin/env python3
"""Spruce suite adapter for DevBridge's PC broker and evidence verdicts."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'DevBridge.pak' / 'tools'))
from broker import Broker, atomic_json


class SpruceBroker(Broker):
    def report(self):
        super().report()
        rows = json.loads((self.out / 'results.json').read_text())
        for row in rows:
            attempts = []
            for path in sorted((self.out / row['cell']).glob('attempt-*/result.json'), key=lambda p: p.stat().st_mtime_ns):
                result = json.loads(path.read_text())
                if any(stage['state'] == 'RUNNING' for stage in result.get('stages', {}).values()):
                    continue
                attempts.append({'verdict': result['verdict'], 'evidence': str(path.relative_to(self.out))})
            row['attempt_count'] = len(attempts)
            row['attempt_history'] = attempts
            row['passed_after_retry'] = row['verdict'] == 'PASS' and any(a['verdict'] != 'PASS' for a in attempts)
        atomic_json(self.out / 'results.json', rows)
        subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'spruce' / 'broker-report.py'),
                        str(self.out)], check=True)

    def __init__(self, *args, **kwargs):
        self.only = kwargs.pop('only', None)
        if self.only:
            re.compile(self.only)
        super().__init__(*args, **kwargs)
        for cell in self.spec['cells']:
            if cell.get('unsupported'):
                self.results[cell['id']] = {'verdict': 'UNSUPPORTED', 'reason': cell['unsupported']}
            elif self.resume:
                previous = sorted((self.out / cell['id']).glob('attempt-*/result.json'), key=lambda p: p.stat().st_mtime_ns)
                if previous:
                    result = json.loads(previous[-1].read_text())
                    if result['verdict'] != 'NOT_RUN':
                        self.results[cell['id']] = result

    def execute_script(self, cell, stage, path):
        fixtures = Path(__file__).resolve().parents[1] / 'spruce'
        for name in ('broker-suite-cell.sh', 'broker-suite-judge.py', 'judge.py', 'lib.sh'):
            (path / (stage + '-' + name)).write_bytes((fixtures / name).read_bytes())
        for name in ('ds-layout.sh', 'check.sh', 'quit.sh'):
            (path / (stage + '-device-' + name)).write_bytes((Path(__file__).resolve().parent / 'device' / name).read_bytes())
        return super().execute_script(cell, stage, path)

    def release(self):
        if self.leases and not self.quarantined:
            directory = self.out / 'final-state'
            directory.mkdir(exist_ok=True)
            command = ['bash', str(Path(__file__).resolve().parents[1] / 'spruce' / 'broker-suite-cell.sh'),
                       'finalize', 'linkplay', 'rcr', 'FC', 'River City Ransom (USA).zip', 'default']
            with (directory / 'restore.log').open('w') as log:
                try:
                    rc = subprocess.run(command, env={**os.environ, 'DEVBRIDGE_ATTEMPT_DIR': str(directory)},
                                        stdout=log, stderr=subprocess.STDOUT, timeout=600).returncode
                except subprocess.TimeoutExpired:
                    rc = 124
            self.restore_rc = rc
            atomic_json(directory / 'result.json', {'returncode': rc, 'expected': 'original ad hoc network, LinkPlay host/client, DevBridge foreground'})
            if rc:
                self.quarantined.update(self.leases)
        super().release()

    def run(self):
        rc = super().run()
        return rc or getattr(self, 'restore_rc', 0)

    def run_cell(self, cell):
        if self.only and not re.search(self.only, cell['id']):
            previous = sorted((self.out / cell['id']).glob('attempt-*/result.json'), key=lambda p: p.stat().st_mtime_ns)
            if previous:
                return json.loads(previous[-1].read_text())
            return {'cell': cell['id'], 'verdict': 'NOT_RUN', 'reason': 'outside requested selection'}
        result = super().run_cell(cell)
        attempt = self.out / result['artifacts']
        evidence = attempt / 'connection-verdict.json'
        log = attempt / 'test.log'
        text = log.read_text(errors='replace') if log.exists() else ''
        if result.get('failure_stage') == 'test' and any(message in text for message in
                ('Permission denied', 'VERIFY reference not found', 'timed out waiting for game', 'Connection timed out',
                 'FAIL: no netplay log', 'FAIL: no pacing windows')):
            result['verdict'] = 'INFRA_FAIL'
            result['reason'] = 'launch, transport, or reference fixture failed; see test.log'
        if cell.get('requires_reference_review') and 'screen does not match' in text:
            result['verdict'] = 'INFRA_FAIL'
            result['reason'] = 'NEEDS_REFERENCE: unapproved DS screen/layout reference'
        if result.get('failure_stage') == 'judge' and evidence.exists():
            result['verdict'] = json.loads(evidence.read_text())['verdict']
        if cell.get('requires_reference_review') and result['verdict'] == 'PASS':
            result['verdict'] = 'INFRA_FAIL'
            result['reason'] = 'NEEDS_REFERENCE: DS navigation still uses unapproved NextUI-derived references'
        if result.get('restore_failed'):
            result['verdict'] = 'INFRA_FAIL'
            result['reason'] = 'baseline restoration failed; devices quarantined'
        atomic_json(attempt / 'result.json', result)
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest')
    parser.add_argument('--out', required=True)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--only', help='cell-ID regex; preserve existing results outside this selection')
    args = parser.parse_args()
    sys.exit(SpruceBroker(args.manifest, args.out, args.resume, only=args.only).run())
