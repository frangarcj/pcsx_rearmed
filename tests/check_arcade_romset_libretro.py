#!/usr/bin/env python3
"""The production libretro core loads System 10, System 11 and Capcom ZN MAME zips.

Builds MAME zips of synthetic ROM files (right sizes and CRC32s, random content,
no game data) for the smallest set of each family, one of them under another
name, and has tests/test_arcade_romset_libretro.c load each twice through the
core: board activation, EEPROM named by set, unload and reload. The caller
supplies a built core and the host executable; temporary files are removed.

  xcrun clang -O2 -std=gnu99 -Ideps/libretro-common/include \
    tests/test_arcade_romset_libretro.c -o build/mac-libretro/test-arcade-romset
  python3 tests/check_arcade_romset_libretro.py \
    --core build/mac-libretro/src/pcsx_rearmed_libretro.dylib \
    --host build/mac-libretro/test-arcade-romset
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_arcade_romset as t  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, default=Path('build/arcade-catalog-tests'))
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    specs = list(t.gen.loadable_sets(t.MAME_SOURCE))
    games = []
    with tempfile.TemporaryDirectory(prefix='arcade-zip-', dir=args.work_dir) as temp:
        root = Path(temp).resolve()
        (root / 'saves').mkdir()
        for family, kind in (('system10', '10'), ('system11', '11'), ('capcom_zn', 'zn')):
            spec = min((s for s in specs if s.family == family), key=t.total_size)
            case = t.Case(spec)
            rompath = root / family
            rompath.mkdir()
            t.write_set_files(case, rompath, 'mygame' if kind == '11' else None)
            name = 'mygame' if kind == '11' else case.name
            games.append(f"{case.name}:{kind}:{rompath / (name + '.zip')}")
        command = [str(args.host.resolve()), str(args.core.resolve()), str(root / 'saves'), *games]
        subprocess.run(command, check=True, timeout=300)


if __name__ == '__main__':
    main()
