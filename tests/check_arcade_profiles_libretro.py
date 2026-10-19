#!/usr/bin/env python3
"""Bounded production-libretro switching test with generated MIPS/C76/Z80 media.

No copyrighted media, captures or source-tree builds. The caller supplies a
built core and tests/test_arcade_profiles_libretro.c executable. Temporary
generated ROM inputs are removed on exit; total disk input is under 128 MiB.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

import check_system10_harness as s10
import check_system11_harness as s11
import check_capcom_zn_harness as zn


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, default=Path('build/arcade-catalog-tests'))
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='libretro-', dir=args.work_dir) as temp:
        root = Path(temp).resolve()
        s10.make_media(root / 's10', 'mrdrilr2')
        s11.create_fixture(root / 's11', 'COH100')
        zn.media(root / 'zn', 'sfex')
        for mode in ('legacy', 'mapped'):
            saves = root / mode
            saves.mkdir()
            command = [str(args.host.resolve()), str(args.core.resolve()),
                       str(root / 's10'), str(root / 's11'), str(root / 'zn'), str(saves), mode]
            subprocess.run(command, check=True, timeout=90)
            for name in ('tekken', 'sfex'):
                assert (saves / (name + '.arcade-nvram')).read_bytes() == b'BAD'


if __name__ == '__main__':
    main()
