#!/usr/bin/env python3
"""Bound replay input times using passive native cycle/frame observations."""
import argparse
import bisect
import csv
import hashlib
import json
from pathlib import Path


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def summarize(inputs, snapshots, frequency):
    with inputs.open() as source:
        events = list(csv.DictReader(source))
    frames, cycles = [], []
    with snapshots.open() as source:
        for row in csv.DictReader(source):
            frame, cycle = int(row['frame']), int(row['cycles'])
            if frames and (frame < frames[-1] or cycle <= cycles[-1]):
                raise ValueError('snapshots must have monotonic frames and increasing cycles')
            frames.append(frame)
            cycles.append(cycle)
    result = []
    previous_frame = 0
    for index, event in enumerate(events):
        frame = int(event['frame'])
        if frame < previous_frame:
            raise ValueError('inputs must be ordered by frame')
        previous_frame = frame
        if frame == 0:
            lower = upper = 0
        else:
            pos = bisect.bisect_left(frames, frame)
            if not pos or pos == len(frames) or frames[pos] != frame:
                raise ValueError(f'input frame {frame} lacks a preceding/first-frame sample')
            lower, upper = cycles[pos-1], cycles[pos]
        midpoint = (lower+upper)/(2*frequency)
        result.append({'event': index+1, **event, 'lower_cycles': lower,
                       'upper_cycles': upper, 'midpoint_seconds': f'{midpoint:.12f}',
                       'old_60hz_seconds': f'{frame/60:.12f}',
                       'old_60hz_minus_midpoint_ms': f'{(frame/60-midpoint)*1000:.6f}'})
    return result, len(frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', required=True, type=Path)
    parser.add_argument('--snapshots', required=True, type=Path)
    parser.add_argument('--frequency', required=True, type=int, help='native CPU cycle frequency')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--summary', required=True, type=Path)
    parser.add_argument('--prune-snapshots', action='store_true',
                        help='remove generated full snapshots after preserving bounds and their hash')
    args = parser.parse_args()
    if args.frequency <= 0:
        parser.error('--frequency must be positive')
    paths = [args.inputs, args.snapshots, args.output, args.summary]
    if len({p.resolve() for p in paths}) != len(paths):
        parser.error('inputs, snapshots, output and summary must have separate paths')
    rows, samples = summarize(args.inputs, args.snapshots, args.frequency)
    if not rows:
        parser.error('input replay is empty')
    report = {'scope': 'passive input-time bounds; midpoints are estimates, not exact input timestamps',
              'frequency': args.frequency, 'events': len(rows), 'snapshot_rows': samples,
              'inputs_sha256': sha256(args.inputs), 'snapshots_sha256': sha256(args.snapshots),
              'snapshots_bytes': args.snapshots.stat().st_size,
              'maximum_bound_width_seconds': max((r['upper_cycles']-r['lower_cycles'])/args.frequency for r in rows),
              'first': rows[0], 'last': rows[-1], 'snapshots_pruned': args.prune_snapshots}
    with args.output.open('w') as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    report['bounds_sha256'] = sha256(args.output)
    args.summary.write_text(json.dumps(report, indent=2)+'\n')
    if args.prune_snapshots:
        args.snapshots.unlink()
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
