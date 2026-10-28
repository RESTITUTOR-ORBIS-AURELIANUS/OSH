#!/usr/bin/env python3
"""Check the pinned source/data and compare every provided-trace result."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def rows_by_case(rows):
    result = {}
    for row in rows:
        key = (Path(row['trace']).name, row['algorithm'], int(row['frames']))
        if key in result:
            raise ValueError(f'duplicate result: {key}')
        result[key] = (int(row['references']), int(row['faults']))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, help='also check a compiled executable')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'release/manifest.json').read_text())
    if (ROOT / 'VERSION').read_text().strip() != manifest['version']:
        raise ValueError('VERSION differs from the release manifest')
    count = 0
    for group in ('source_sha256', 'data_sha256', 'reference_results_sha256'):
        for relative, expected in manifest[group].items():
            actual = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f'changed pinned file: {relative}')
            count += 1
    print(f"{manifest['tag']}: {count} pinned file hashes match")
    if args.binary:
        binary = args.binary.resolve()
        files = [ROOT / 'data' / name for name in ('sort1.txt', 'lu.txt', 'mm16.txt', 'mm32.txt')]
        command = [str(binary), '--algorithm', 'all', '--frames',
                   ','.join(map(str, manifest['default_frames'])), '--format', 'csv', *map(str, files)]
        completed = subprocess.run(command, capture_output=True, text=True, check=True, timeout=180)
        actual = rows_by_case(csv.DictReader(io.StringIO(completed.stdout)))
        with (ROOT / 'results/provided.csv').open(newline='') as stream:
            expected = rows_by_case(csv.DictReader(stream))
        if actual != expected:
            mismatches = [str(key) for key in sorted(set(actual) | set(expected))
                          if actual.get(key) != expected.get(key)]
            raise ValueError('benchmark regression: ' + ', '.join(mismatches[:10]))
        values = [v[1] for k, v in actual.items() if k[1] == 'adaptive']
        if len(values) != manifest['provided_cases'] or sum(values) / len(values) != manifest['provided_mean_faults']:
            raise ValueError('AER aggregate differs from the pinned result')
        print(f'{len(actual)} algorithm/trace/frame cases match; AER mean faults = {sum(values)/len(values):.2f}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
