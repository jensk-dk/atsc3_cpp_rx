#!/usr/bin/env python3
"""Checks a cvlc -vv log of the atsc3 plugin against an expectations file (spec 5.19.5 harness 1).

Expectation keys (all optional): programs_min, selected (substring of the 'selected' line), es (substrings of
'es added' lines that must appear), es_absent, decoders (substrings that must appear anywhere, e.g. 'codec (hevc)
started'), first_picture (bool), min_video_fragments, min_audio_fragments, min_subtitle_fragments, epg_min_events,
alerts_min, eof (bool: 'end of capture' expected), forbid (substrings that must not appear; defaults to the plugin's
and the input's error lines), no_decoder (codecs expected to lack a decoder: informational)."""
import json
import re
import sys

log_path, expected_path = sys.argv[1], sys.argv[2]
with open(expected_path, encoding='utf-8') as f:
    exp = json.load(f)
with open(log_path, encoding='utf-8', errors='replace') as f:
    lines = f.read().splitlines()

failures = []
notes = []

def find(sub):
    return [l for l in lines if sub in l]

programs = 0
for l in find('atsc3: programs:'):
    programs = max(programs, int(l.rsplit(':', 1)[1]))
if 'programs_min' in exp and programs < exp['programs_min']:
    failures.append(f'programs {programs} < {exp["programs_min"]}')
notes.append(f'programs {programs}')

sel = find('atsc3: selected')
if 'selected' in exp:
    if not any(exp['selected'] in l for l in sel):
        failures.append(f'no "selected ...{exp["selected"]}" line ({len(sel)} selections)')
notes.append('selected: ' + '; '.join(l.split('atsc3: selected', 1)[1].strip() for l in sel))

es_lines = [l.split('atsc3: es added', 1)[1].strip() for l in find('atsc3: es added')]
for want in exp.get('es', []):
    if not any(want in l for l in es_lines):
        failures.append(f'missing ES "{want}" (have: {es_lines})')
for bad in exp.get('es_absent', []):
    if any(bad in l for l in es_lines):
        failures.append(f'unexpected ES "{bad}"')
notes.append('es: ' + '; '.join(es_lines))

for want in exp.get('decoders', []):
    if not find(want):
        failures.append(f'missing "{want}"')
if exp.get('first_picture') and not find('Received first picture'):
    failures.append('no "Received first picture"')

frag = {}
stats = find('atsc3: stats')
if stats:
    for m in re.finditer(r'(video|audio|subtitles) (\d+) frag/(\d+) samples', stats[-1]):
        frag[m.group(1)] = (int(m.group(2)), int(m.group(3)))
for kind in ('video', 'audio', 'subtitle'):
    key = f'min_{kind}_fragments'
    if key in exp:
        have = frag.get('subtitles' if kind == 'subtitle' else kind, (0, 0))[0]
        if have < exp[key]:
            failures.append(f'{kind} fragments {have} < {exp[key]}')
notes.append('fragments: ' + ', '.join(f'{k} {v[0]}/{v[1]} samples' for k, v in frag.items()))

events = 0
for l in find('atsc3: epg:'):
    m = re.search(r'(\d+) events pushed to (\d+) programs', l)
    if m:
        events = max(events, int(m.group(1)))
if exp.get('epg_min_events', 0) > events:
    failures.append(f'epg events {events} < {exp["epg_min_events"]}')
notes.append(f'epg events {events}')

alerts = find('atsc3: alert ')
if exp.get('alerts_min', 0) > len(alerts):
    failures.append(f'alerts {len(alerts)} < {exp["alerts_min"]}')
notes.append(f'alerts {len(alerts)}')

if exp.get('eof') and not find('atsc3: end of capture'):
    failures.append('no "end of capture"')

for bad in exp.get('forbid', ['atsc3: error', 'main input error', 'demux error']):
    hits = find(bad)
    if hits:
        failures.append(f'forbidden "{bad}": {hits[0][:160]}')

for codec in exp.get('no_decoder', []):
    if find('no suitable decoder module') or find("codec not supported"):
        notes.append(f'{codec}: no decoder, as expected')

print('checks: ' + ' | '.join(notes))
if failures:
    print('FAIL: ' + '; '.join(failures))
    sys.exit(1)
print('PASS')
