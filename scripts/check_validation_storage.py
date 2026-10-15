#!/usr/bin/env python3
"""Refuse a build/test batch that would leave less than 10 GiB free."""
import argparse
import json
from pathlib import Path
import shutil

MIN_FREE_BYTES = 10 * 1024**3


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
    allowed = free - reserve >= MIN_FREE_BYTES
    print(json.dumps({'allowed': allowed, 'free_bytes': free,
                      'estimated_new_bytes': reserve,
                      'minimum_remaining_bytes': MIN_FREE_BYTES,
                      'remaining_after_estimate_bytes': free-reserve}, indent=2))
    if not allowed:
        print('Storage guard BLOCKED: clean disposable outputs before this batch.')
    return 0 if allowed else 1


if __name__ == '__main__':
    raise SystemExit(main())
