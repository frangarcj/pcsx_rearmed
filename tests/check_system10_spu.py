#!/usr/bin/env python3
"""Compare System 10 serial/worker SPU output through the production core."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import check_system10_drc as fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='system10-spu-') as temp:
        media = Path(temp) / 'MEMM'
        fixture.make_media(media, 'MEMM')
        results = []
        for threaded in (0, 1, 1):
            command = [str(args.host.resolve()), str(args.core.resolve()), str(media), str(threaded)]
            run = subprocess.run(command, check=True, timeout=90, capture_output=True, text=True)
            print(run.stdout + run.stderr, end='', flush=True)
            result = re.findall(r'RESULT .*', run.stderr)
            assert len(result) == 1
            results.append(result)
        assert results[0] == results[1] == results[2], results
        print('System10 SPU serial/threaded repeat: identical sample counts and hashes PASS')


if __name__ == '__main__':
    main()
