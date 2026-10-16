#!/usr/bin/env python3
"""Screen an upstream (PrimeDecomp) delta for changes that actually alter behaviour.

The decomp's commit stream is dominated by matching work: symbol/split/config
churn, function reordering, `#pragma inline_max_size` twiddles, and the field
rename pass (`xc_anyZThreshold` -> `mAnyZThreshold`). None of that changes what
the code does, but it drowns the handful of real fixes a port cares about.

This script normalises the away:

  * offset-prefixed identifiers, including bitfield members
    (`x1c_maxCollisionCycles` and `x30_24_canShowAuxMuzzleEffect` -> `m_...`)
  * already-renamed members (`mWallElasticLinear` -> `m_wallelasticlinear`)
  * whitespace, blank lines, comments and `{`/`}` lines
  * function reordering, by comparing line *multisets* rather than positions

Whatever survives in a file is a real line-level difference in executed code,
printed as `-`/`+` normalized lines.

Usage (from the repo root, with the upstream remote fetched):

    git fetch origin main
    python3 tools/upstream_screen.py                 # every src/**/*.cpp
    python3 tools/upstream_screen.py --paths src/Kyoto/Particles src/MetroidPrime/Player
    python3 tools/upstream_screen.py --top 10        # only the 10 noisiest files
    python3 tools/upstream_screen.py --up origin/main --base $(git merge-base origin/main HEAD)

Limitations: renames that do not follow the `xNN_name`/`mName` convention
(e.g. `mCurrentBeam` -> `mEquippedBeamId`) still look like changes, and code
moved between files shows up as removal here plus addition there. Treat the
output as a shortlist to read, not as a verdict.
"""

import argparse
import collections
import re
import subprocess
import sys

IDENT_RE = re.compile(r'\bx[0-9a-fA-F]+_(\w+)')
FIELD_RE = re.compile(r'\bm([A-Z]\w*)')
BITFIELD_RE = re.compile(r'\bm_\d+_')
OFFSET_RE = re.compile(r'\bx[0-9a-fA-F]{4,}\b|\bfn_[0-9a-fA-F]+\b')
COMMENT_RE = re.compile(r'//.*$|/\*.*?\*/')


def normalize(text):
    """Collapse formatting, renames and reordering so only real change remains."""
    out = []
    for line in text.split('\n'):
        line = COMMENT_RE.sub('', line)
        line = IDENT_RE.sub(lambda m: 'm_' + m.group(1).lower(), line)
        line = FIELD_RE.sub(lambda m: 'm_' + m.group(1).lower(), line)
        line = BITFIELD_RE.sub('m_', line)
        line = OFFSET_RE.sub('off', line)
        line = re.sub(r'\s+', '', line).strip(';')
        if line and line not in ('{', '}'):
            out.append(line)
    return out

def git(*args):
    proc = subprocess.run(['git', *args], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f'git {" ".join(args)} failed:\n{proc.stderr.strip()}')
    return proc.stdout


def git_show(ref, path):
    """Contents of path at ref, or '' when the file does not exist there."""
    proc = subprocess.run(['git', 'show', f'{ref}:{path}'], capture_output=True, text=True)
    return proc.stdout if proc.returncode == 0 else ''


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--up', default='origin/main', help='upstream ref (default origin/main)')
    ap.add_argument('--base', default=None,
                    help='base commit (default: git merge-base <up> HEAD)')
    ap.add_argument('--paths', nargs='*', default=['src'],
                    help='git pathspecs to consider (default: src)')
    ap.add_argument('--top', type=int, default=0, help='show only the N noisiest files')
    ap.add_argument('--ext', default='.cpp', help='file suffix to screen (default .cpp)')
    ap.add_argument('--max-lines', type=int, default=12,
                    help='changed lines printed per side per file (default 12)')
    ap.add_argument('--keep-preproc', action='store_true',
                    help='keep preprocessor/version-gate lines (dropped by default)')
    args = ap.parse_args()

    base = args.base or git('merge-base', args.up, 'HEAD').strip()
    print(f'{args.up} since {base[:9]}  ({git("rev-list", "--count", f"{base}..{args.up}").strip()} commits)')

    paths = git('diff', '--name-only', base, args.up, '--', *args.paths).split('\n')
    files = [f for f in paths if f.endswith(args.ext)]

    results = []
    for path in files:
        old = git_show(base, path)
        new = git_show(args.up, path)
        if not new:
            continue
        c_old = collections.Counter(normalize(old))
        c_new = collections.Counter(normalize(new))
        if c_old == c_new:
            continue
        removed = list((c_old - c_new).elements())
        added = list((c_new - c_old).elements())
        if not args.keep_preproc:
            # version gates (#if VERSION >= VERSION_R3IJ_00 and friends) are noise here
            removed = [l for l in removed if not l.startswith('#')]
            added = [l for l in added if not l.startswith('#')]
        if removed or added:
            results.append((len(removed) + len(added), path, removed, added))

    results.sort(reverse=True)
    if args.top:
        results = results[:args.top]

    print(f'{len(results)} of {len(files)} {args.ext} files have non-mechanical change\n')
    for total, path, removed, added in results:
        print(f'=== {path}  ({total} changed normalized lines)')
        for line in removed[:args.max_lines]:
            print('  - ' + line[:160])
        for line in added[:args.max_lines]:
            print('  + ' + line[:160])
        print()


if __name__ == '__main__':
    main()
