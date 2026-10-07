#!/usr/bin/env python3
"""Checks the C++ ANUV flattening against the reference evaluator.

Usage: anuv_check.py <port_remastered_anuv_tool> <anuv.py's directory> <models dir>

Runs the tool over every SMDL/CMDL in the directory that has an ANUV chunk and
compares each flattened transform with anuv.py's at the tool's times. Prints the
counts: models, entries flattened, entries skipped by reason, mismatches.
"""
import collections
import glob
import os
import subprocess
import sys

tool, anuv_dir, models = sys.argv[1:4]
sys.path.insert(0, anuv_dir)
import anuv  # noqa: E402

files = [f for f in sorted(glob.glob(os.path.join(models, '*')))
         if f.endswith(('.SMDL', '.CMDL')) and b'ANUV' in open(f, 'rb').read()]
ref = {}
for f in files:
    a = anuv.parse_file(f)
    if a is not None:
        ref[f] = a
out = collections.defaultdict(list)
for i in range(0, len(files), 50):
    res = subprocess.run([tool] + files[i:i + 50], capture_output=True, text=True, check=True)
    for line in res.stdout.splitlines():
        out[line.split()[0]].append(line.split()[1:])
skips = collections.Counter()
flattened = bad = 0
worst = 0.0
unreadable = [l for l in out['MODEL'] if l[1] in ('unreadable', 'unparsed', 'none')]
skipped = set()
for l in out['SKIP']:
    skips[' '.join(l[3:])] += 1
    skipped.add((l[0], int(l[1])))
for l in out['EVAL']:
    f, e, k, t = l[0], int(l[1]), int(l[2]), float(l[3])
    got = [float(x) for x in l[4:]]
    want = anuv.entry_transforms(ref[f], t)[e][k]
    # Compare modulo wrap of U/V translation: a curve that is a whole turn apart
    # at a loop seam reads the same texel.
    d = max(abs(x - y) for x, y in zip(got, want))
    worst = max(worst, d)
    if d >= 1e-4:
        bad += 1
        if bad <= 10:
            print('MISMATCH', os.path.basename(f), e, k, t, d)
nentries = sum(len(r['entries']) for r in ref.values())
print('models %d (reference parsed %d), unreadable %d' % (len(files), len(ref), len(unreadable)))
print('entries %d: flattened %d, skipped %d' % (nentries, nentries - len(skipped), len(skipped)))
for why, n in skips.most_common():
    print('  skipped: %-45s %d' % (why, n))
print('transform samples compared %d, mismatches %d, worst %.2e' % (len(out['EVAL']), bad, worst))
