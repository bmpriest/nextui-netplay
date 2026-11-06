#!/usr/bin/env python3
"""Compare four archived network/transport suites, preserving missing data."""
import argparse,csv,json
from pathlib import Path

CONFIGS=('adhoc-tcp','adhoc-hybrid','house-tcp','house-hybrid')
KEYS=('avg_fps','slowest_10s_fps','longest_freeze_ms','checks_failed','checks_total','state_resends','coverage_s','window_s')

def value(v,key):
    n=v.get(key)
    return '—' if n is None else (f'{n:.1f}' if key in KEYS[:3] else str(n))

def main(root):
    root=Path(root)
    runs={}
    flat=[]
    names={}
    for config in CONFIGS:
        file=root/config/'results.json'
        if not file.exists():continue
        rows=json.loads(file.read_text());runs[config]={r['cell']:r for r in rows}
        csvfile=root/config/'comparison.csv'
        if csvfile.exists():
            csvrows=list(csv.DictReader(csvfile.open()))
            spec=json.loads((root/config/'suite.json').read_text())['manifest']
            for cell in spec['cells']:
                args=cell['test']; protocol,test,system,rom,core=args[3:8]
                entry=next((v for v in csvrows if v['system']==system and v['protocol']==('LinkPlay' if protocol=='linkplay' else 'RetroArch over Netplay') and
                            ((core=='default' and ('paired' in v['core_or_mode'] if system in ('GB','GBC') else True)) or
                             core.lower().replace('mednafen_','') in v['core_or_mode'].lower())),None)
                if entry:names[cell['id']]=entry

        for row in rows:
            metric=row.get('telemetry',{})
            proof='not applicable (RetroArch transport unchanged)'
            if row['cell'].startswith('linkplay') and row.get('artifacts'):
                logs=[root/config/row['artifacts']/'measurement'/f'{d}.log' for d in ('b','h')]
                text=[p.read_text(errors='replace') if p.exists() else '' for p in logs]
                marker='UDP disabled - input/hash staying on TCP' if config.endswith('-tcp') else 'peer UDP endpoint port='
                proof=('TCP forced on both devices' if config.endswith('-tcp') else 'UDP negotiated on both devices') if all(marker in t for t in text) else 'UNCONFIRMED'
            flat.append({'network':config.split('-')[0],'transport':config.split('-')[1],
                         'cell':row['cell'], 'game':names.get(row['cell'],{}).get('game',row['cell']),
                         'protocol':names.get(row['cell'],{}).get('protocol',row['cell'].split('-')[0]),
                         'core_or_mode':names.get(row['cell'],{}).get('core_or_mode',''), 'verdict':row['verdict'],**{k:metric.get(k) for k in KEYS},
                         'transport_proof':proof,'evidence':f"{config}/{row.get('artifacts','')}"})
    allcells=list(dict.fromkeys(r['cell'] for r in flat))
    lines=['Each pair below is **TCP → hybrid**. Frames per second and milliseconds are observed log estimates; checks and state resends are gameplay-window counts.','',
           '| Network | Protocol | Game / core or mode | Avg FPS | Slowest 10s FPS | Longest freeze ms | Checks failed | State resends | FPS coverage TCP → hybrid | Result TCP → hybrid |',
           '|---|---|---|---:|---:|---:|---|---|---|---|']
    for network in ('adhoc','house'):
        for cell in allcells:
            rows=[runs.get(f'{network}-{mode}',{}).get(cell,{}) for mode in ('tcp','hybrid')]
            metrics=[r.get('telemetry',{}) for r in rows]
            columns=[' → '.join(value(m,k) for m in metrics) for k in KEYS[:3]]
            checks=' → '.join('—' if m.get('checks_total') is None else f"{m['checks_failed']}/{m['checks_total']}" for m in metrics)
            states=' → '.join(value(m,'state_resends') for m in metrics)
            coverage=' → '.join('—' if m.get('window_s') is None else f"{m['coverage_s']:.1f}/{m['window_s']:.1f}s" for m in metrics)
            verdict=' → '.join(r.get('verdict','NOT_RUN')+(' (retry)' if r.get('passed_after_retry') else '') for r in rows)
            name=names.get(cell,{})
            protocol=name.get('protocol',cell.split('-')[0])
            game=name.get('game',cell)+' / '+name.get('core_or_mode','')
            label='NextUI Brick Wi-Fi' if network=='adhoc' else 'House Wi-Fi'
            lines.append(f"| {label} | {protocol} | {game} | {' | '.join(columns)} | {checks} | {states} | {coverage} | {verdict} |")
    lines+=['','Full game names, cores, per-device samples, coverage, and evidence:', '']
    lines += [f'- [{config}]({config}/report.md)' for config in CONFIGS]
    lines += ['', '[All rows as CSV](comparison.csv). [Bounded UDP navigation samples](udp-navigation-proof.json).', '', 'Build and source records: [shim/source hashes](build-hashes.txt), [test commit](test-commit.txt), [source changes at launch](source.diff), [controller journal](controller.log).', '',
              'The same compiled shim is used for TCP and hybrid; TCP sets NETLINK_TEST_NO_UDP. RetroArch uses its separate unchanged protocol and is a control, not a hybrid implementation. Serial cable data and state transfers remain on TCP in hybrid mode. UDP negotiation confirms selection, not a count of UDP packets; per-send fallback to TCP remains possible. Each configuration uses the same test recipe; random gameplay inputs are not identical between runs. This is one suite per configuration, not a repeated statistical trial. No artificial packet loss was injected. Differences describe these physical Wi-Fi runs and do not by themselves prove loss resilience.', '',
              '— means no usable measurement. DS is omitted. Unsupported cases remain visible. Failed navigation is not substituted with menu or solo FPS. FPS coverage shows observed pacing duration against the requested gameplay interval; partial pre-disconnect rates are not full-session performance. Missing tails are not assumed to run at the last known speed. Instrumentation uses existing logs, and its overhead has not been independently validated.', '', 'Transport selection evidence:', '']
    lines += [f"- {r['network']}-{r['transport']} / {r['cell']}: {r['transport_proof']}" for r in flat if r['cell'].startswith('linkplay')]
    lines += ['', 'Failure details:', '']
    for config,rows in runs.items():
        for cell,row in rows.items():
            if row['verdict'] in ('PASS','UNSUPPORTED','NOT_RUN'): continue
            reason=row.get('reason')
            artifact=row.get('artifacts')
            if artifact:
                attempt=root/config/artifact
                judgment=attempt/'connection-verdict.json'
                if not reason and judgment.exists():
                    verdict=json.loads(judgment.read_text())
                    reason=verdict.get('reason') or '; '.join(d+': '+v.get('reason','') for d,v in verdict.get('devices',{}).items() if v.get('reason'))
                log=attempt/'test.log'
                if not reason and log.exists():
                    text=log.read_text(errors='replace').splitlines()
                    reason=next((line for line in text if 'screen does not match' in line or 'FAIL:' in line),None)
            reason=reason or 'See archived verdict and stage logs.'
            lines.append(f"- {config} / {cell}: {reason}")
    (root/'report.md').write_text('\n'.join(lines)+'\n')
    if flat:
        with (root/'comparison.csv').open('w',newline='') as file:
            writer=csv.DictWriter(file,fieldnames=list(flat[0]));writer.writeheader();writer.writerows(flat)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('root');main(parser.parse_args().root)
