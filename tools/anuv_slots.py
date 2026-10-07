#!/usr/bin/env python3
"""Counts, over every model with an ANUV, how the converter's texcoord slots fall out.

Usage: anuv_slots.py <port_remastered_anuv_tool> <anuv.py's directory> <models dir>
                     <mats.tsv> <meshes.tsv>

Needs no retail files beyond the extracted models and the local material index.
For each material a mesh's entry reaches, the four PBR maps (BCLR, METL, NMAP,
ICAN) read their authored texcoord k through the AUVI to a source set; the
entry's transform k moves or not. Prints the materials that are animated, the
ones where a source set is read through differing transforms (what needed a
texcoord copy), and the extra slots they need. Whether a retail descriptor has
a texcoord to spare isn't in the index, so "out of slots" is the worst case: a
material that would need more than eight with every declared set counted.
"""
import collections
import glob
import os
import subprocess
import sys

tool, anuv_dir, models, mats_tsv, meshes_tsv = sys.argv[1:6]
sys.path.insert(0, anuv_dir)
import anuv  # noqa: E402

files = [f for f in sorted(glob.glob(os.path.join(models, '*')))
         if f.endswith(('.SMDL', '.CMDL')) and b'ANUV' in open(f, 'rb').read()]
ref = {f: a for f in files if (a := anuv.parse_file(f)) is not None}

moving = collections.defaultdict(set)  # (file, entry) -> transform indices that move
for i in range(0, len(files), 50):
    res = subprocess.run([tool] + files[i:i + 50], capture_output=True, text=True, check=True)
    for line in res.stdout.splitlines():
        p = line.split()
        if p[0] != 'EVAL':
            continue
        got = [float(x) for x in p[4:]]
        if max(abs(a - b) for a, b in zip(got, [1, 0, 0, 0, 0, 1, 0, 0])) > 1e-6:
            moving[(p[1], int(p[2]))].add(int(p[3]))

meshmat = {}
for line in open(meshes_tsv):
    c = line.rstrip('\n').split('\t')
    meshmat[c[0]] = [int(x) for x in c[3].split(',')]
rows = collections.defaultdict(list)
for line in open(mats_tsv):
    c = line.rstrip('\n').split('\t')
    rows[c[0]].append(c[7].split() if len(c) > 7 else [])

total = animated = hit = needs = starved = 0
extra = collections.Counter()
for f, a in ref.items():
    name = os.path.basename(f)
    mats = rows.get(name, [])
    mm = meshmat.get(name[:36], [])
    entry_of = {}
    for mesh, e in enumerate(a['matmap']):
        if e != 0xff and mesh < len(mm):
            entry_of.setdefault(mm[mesh], e)
    for mat, e in entry_of.items():
        if mat >= len(mats):
            continue
        total += 1
        params = dict(p.split('=', 1) for p in mats[mat] if '=' in p)
        auvi = [int(x) for x in params['AUVI'].split(',')] if 'AUVI' in params else None
        mv = moving.get((f, e), set())
        reads = []  # (source set, authored coord)
        for tag in ('BCLR', 'METL', 'NMAP', 'ICAN'):
            if tag in params and '@' in params[tag]:
                k = int(params[tag].split('@')[1])
                src = auvi[k] if auvi and k < 3 and 0 <= auvi[k] else k
                reads.append((src, k))
        if any(k in mv for _, k in reads):
            animated += 1
        slots = {}
        for src, k in reads:
            slots.setdefault(src, set()).add(k if k in mv else -1)
        copies = sum(len(t) - 1 for t in slots.values())
        if copies:
            hit += 1
            extra[copies] += 1
        if len(slots) + copies > 8:
            starved += 1
print('materials reached by an ANUV entry: %d' % total)
print('animated (a map reads a moving transform): %d' % animated)
print('shared source set with differing transforms: %d, extra slots %s' % (hit, dict(extra)))
print('over eight slots even from a clean start: %d' % starved)
