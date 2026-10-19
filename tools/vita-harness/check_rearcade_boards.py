#!/usr/bin/env python3
"""Bounded ARMv7 checks of both executables in the production ReArcade VPK.

Uses generated MIPS/H8/C76/Z80 media, one private COW runtime and linked
profile directories. This checks executable integration, not real-game
compatibility. Retains reports, small PNGs, hashes and synthetic NVRAM only.
Run the repository storage guard before invoking this batch (160 MiB reserve).
Only one generated board profile is kept at a time.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
import check_system10_harness as s10
import check_system11_harness as s11
import check_capcom_zn_harness as zn
import check_system12_drc as s12


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--firmware-fs', type=Path, required=True)
    parser.add_argument('--shader-compiler', type=Path, required=True)
    parser.add_argument('--config-template', type=Path)
    parser.add_argument('--vpk', type=Path, default=ROOT / 'pcsx-rearcade.vpk')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--renderer', choices=('neon', 'vitagl', 'both'), default='both')
    parser.add_argument('--case', action='append', choices=('s12-4', 's12-16', 's10', 's11', 'zn'))
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error('output directory already exists')
    args.output_dir.mkdir(parents=True)
    h = load('rearcade_guest', ROOT / 'tools/vita-harness/check_system12_gameplay_vita3k.py')
    checker = load('rearcade_shader', ROOT / 'tools/vita-harness/check_vita3k.py')
    drc = load('rearcade_drc', ROOT / 'tools/vita-harness/check_system12_drc_vita3k.py')
    h.TITLE_ID = 'PCRA00001'
    records = []
    summary = {'passed': False, 'scope': 'synthetic production ARMv7 executable integration',
               'vpk_sha256': digest(args.vpk),
               'host_sha256': digest(h.emulator_executable(args.emulator)), 'cases': records}

    with tempfile.TemporaryDirectory(prefix='rearcade-runtime-', dir=ROOT / 'build') as temporary:
        base = Path(temporary)
        emulator, fs, portable = h.read_cli(base, args)
        checker.install_local_shader_compiler(emulator, args.shader_compiler)
        config = portable / 'config.yml'
        text = config.read_text()
        if '\ndisable-surface-sync:' not in '\n' + text:
            raise RuntimeError('host config has no disable-surface-sync key')
        config.write_text('\n'.join('disable-surface-sync: false'
                                   if line.startswith('disable-surface-sync:') else line
                                   for line in text.splitlines()) + '\n')
        shutil.copy2(config, args.output_dir / 'vita3k-config.yml')
        if not h.install_vpk(emulator, args.vpk.resolve(), h.TITLE_ID,
                             args.output_dir / 'install.log', args.timeout):
            raise RuntimeError('production VPK installation failed')
        app = fs / 'ux0/app' / h.TITLE_ID
        (app / 'eboot.bin').rename(app / 'eboot_vitagl.bin')
        profiles = base / 'profiles'
        profiles.mkdir()
        mounted = fs / 'ux0/data/profiles'
        mounted.mkdir(parents=True)
        replay = 'frame,control,pressed\n0,p1button1,1\n2,p1button1,0\n3,coin1,1\n4,coin1,0\n5,p1button1,1\n'
        cases = [('s12-4', 8, 88200, None), ('s12-16', 8, 88200, None),
                 ('s10', 8, 66075, 'IN1=ffffffef'),
                 ('s11', 8, 88200, 'inputs=ff/ff/ef/ff'),
                 ('zn', 8, 24038, 'inputs=ef/ff/ff/ff/ff/ff')]
        renderers = ('neon', 'vitagl') if args.renderer == 'both' else (args.renderer,)
        try:
            for name, frames, rate, held in cases:
                if args.case and name not in args.case:
                    continue
                profile = profiles / name
                if name.startswith('s12-'):
                    profile.mkdir()
                    s12.make_regions(profile, int(name.split('-')[1]), vita_signature=True)
                elif name == 's10':
                    s10.make_media(profile, 'mrdrilr2')
                elif name == 's11':
                    s11.create_fixture(profile, 'COH100')
                else:
                    zn.media(profile, 'sfex')
                if held:
                    (profile / 'inputs.csv').write_text(replay)
                (mounted / name).symlink_to(profile, target_is_directory=True)
                inputs = {f.name: {'bytes': f.stat().st_size, 'sha256': digest(f)}
                          for f in profile.iterdir() if f.is_file()}
                for renderer in renderers:
                    selected = app / f'eboot_{renderer}.bin'
                    (app / 'eboot.bin').unlink(missing_ok=True)
                    drc.copy_runtime_file(selected, app / 'eboot.bin')
                    case = args.output_dir / f'{renderer}-{name}'
                    case.mkdir()
                    report_guest = f'ux0:data/{renderer}-{name}.txt'
                    guest_args = (f'pcsx-vita, --{"system12" if held is None else "arcade"}, '
                                  f'ux0:data/profiles/{name}, --frames, {frames}, --gpu-thread, '
                                  f'--report-arcade, {report_guest}')
                    expected = [f'pcsx-vita: system12-report PASS path={report_guest}']
                    if held is None:
                        ram = 4 if name == 's12-4' else 16
                        guest_args += f', --verify-system12-drc, {ram}'
                        expected.append(f'pcsx-vita: system12-drc PASS cpu=ari64 ram={ram}M')
                    else:
                        guest_args += f', --inputs, ux0:data/profiles/{name}/inputs.csv'
                        expected.append('pcsx-vita: inputs replay PASS')
                    if renderer == 'vitagl':
                        expected.append('pcsx-vita: vitaGL shutdown PASS (0)')
                    screenshot_root = portable / 'screenshots'
                    before = set(screenshot_root.rglob('*.png')) if screenshot_root.exists() else set()
                    ok, issues = h.run_guest(emulator, guest_args, case / 'vita3k.log', args.timeout,
                                             tuple(expected), extra_env={'VITA3K_AUTO_SCREENSHOT': '0.05,0.1'})
                    screenshots = []
                    after = set(screenshot_root.rglob('*.png')) if screenshot_root.exists() else set()
                    for index, source in enumerate(sorted(after - before)[:2]):
                        target = case / f'native-{index}.png'
                        shutil.copy2(source, target)
                        screenshots.append({'file': target.name, 'sha256': digest(target)})
                    report = fs / 'ux0/data' / f'{renderer}-{name}.txt'
                    report_text = report.read_text() if report.is_file() else ''
                    if report.is_file():
                        shutil.copy2(report, case / 'report.txt')
                    log = (case / 'vita3k.log').read_text()
                    if held and held not in report_text:
                        ok = False; issues['mapped player input absent'] = 1
                    audio = re.search(r'arcade-audio rate=(\d+) frames=(\d+) peak=(\d+) nonzero=(\d+) errors=(\d+)', log)
                    if not audio or int(audio[1]) != rate or not int(audio[2]) or int(audio[5]):
                        ok = False; issues['audio rate/progress/error'] = 1
                    if name in ('s11', 'zn') and audio and not int(audio[3]):
                        ok = False; issues['instruction-programmed sound silent'] = 1
                    if re.search(r'\b(?:faults?|unsupported|unmapped_reads|unmapped_writes)=[1-9]', report_text):
                        ok = False; issues['board diagnostics'] = 1
                    record = {'renderer': renderer, 'board': name, 'frames': frames, 'passed': ok,
                              'issues': issues, 'eboot_sha256': digest(selected),
                              'input_hashes': inputs,
                              'screenshots': screenshots,
                              'audio': list(map(int, audio.groups())) if audio else None}
                    records.append(record)
                    (args.output_dir / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
                    print(json.dumps(record), flush=True)
                    if not ok:
                        raise RuntimeError(f'production executable failed: {renderer}/{name}')
                (mounted / name).unlink()
                shutil.rmtree(profile)  # Locally generated test media only.
            summary['passed'] = True
        finally:
            nvram = fs / 'ux0/data/pcsx-rearcade'
            for image in nvram.rglob('*.arcade-nvram') if nvram.exists() else ():
                destination = args.output_dir / 'synthetic-nvram' / image.name
                destination.parent.mkdir(exist_ok=True)
                shutil.copy2(image, destination)
            (args.output_dir / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
