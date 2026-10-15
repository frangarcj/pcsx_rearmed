#!/usr/bin/env python3
"""Refuse batches with pending raw captures or less than 10 GiB remaining."""
import argparse
import json
from pathlib import Path
import shutil

MIN_FREE_BYTES = 10 * 1024**3
RAW_CAPTURE_SUFFIXES = {'.wav', '.ram', '.vram', '.ppm'}


def pending_captures(artifacts):
    """Inspect generated evidence only; original media is never a cleanup target."""
    return sorted(p for p in artifacts.rglob('*')
                  if p.suffix.lower() in RAW_CAPTURE_SUFFIXES
                  and not p.is_symlink() and p.is_file())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reserve-mib', required=True, type=int,
                        help='estimated total new output, including scratch files')
    parser.add_argument('--path', type=Path, default=Path.cwd(),
                        help='existing directory on the output volume')
    args = parser.parse_args()
    if args.reserve_mib < 0:
        parser.error('--reserve-mib must be nonnegative')
    reserve = args.reserve_mib * 1024**2
    free = shutil.disk_usage(args.path).free
    captures = pending_captures(Path(__file__).resolve().parents[1] / 'artifacts')
    allowed = free - reserve >= MIN_FREE_BYTES and not captures
    print(json.dumps({'allowed': allowed, 'free_bytes': free,
                      'estimated_new_bytes': reserve,
                      'minimum_remaining_bytes': MIN_FREE_BYTES,
                      'remaining_after_estimate_bytes': free-reserve,
                      'pending_raw_capture_bytes': sum(p.stat().st_size for p in captures),
                      'pending_raw_captures': [str(p) for p in captures]}, indent=2))
    if not allowed:
        print('Storage guard BLOCKED: clean disposable outputs before this batch.')
        if captures:
            print('Retain observations/hashes and prune the previous batch before starting another.')
    return 0 if allowed else 1


if __name__ == '__main__':
    raise SystemExit(main())
