#!/usr/bin/env python3
"""Audit original Percussion Freaks 4th Mix KAA media; does not enable execution."""
import argparse
from dataclasses import replace
import json
from pathlib import Path
import re
import shutil
import subprocess
import prepare_mame_psx as mame
import capcom_zn_boards as zn

SET = 'pcnfrk4mk'
DISK_SHA = '6c9084773c6964e794bd096822da4413aa7305ef'
FILES = {'maincpu:rom': 'main.bin', 'h8_response': 'h8_response.bin',
         'cassette:game:eeprom': 'cassette.bin', 'cassette:game:id': 'serial.bin'}
DEVICE_FILES = {'k573dio': {'digital_id': 'digital_id.bin'},
                'k573msu': {'tmpr3927': 'msu_flash.bin', 'm48t58y': 'msu_timekeeper.bin'}}


def source_profile(source, name=SET, bios='std'):
    if name != SET or bios != 'std':
        raise mame.PreparationError('only Percussion Freaks 4th Mix KAA / standard BIOS is reviewed')
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise mame.PreparationError('MAME source pin mismatch')
    paths = ['src/mame/konami/' + n for n in ('ksys573.cpp', 'k573cass.cpp', 'zs01.cpp', 'zs01.h',
                                            'k573dio.cpp', 'k573msu.cpp')]
    if subprocess.run(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--', *paths]).returncode:
        raise mame.PreparationError('reviewed System 573 digital source differs from pin')
    text = (source / paths[0]).read_text()
    row = zn._game_rows(text)[name]
    if tuple(row[k] for k in ('parent', 'machine', 'input', 'state', 'init', 'flags')) != (
            'pcnfrk4m', 'drmn4m', 'drmn', 'ksys573_state', 'empty_init', 'MACHINE_IMPERFECT_SOUND'):
        raise mame.PreparationError('digital source registration changed')
    machine = zn._function_body(text, r'void\s+ksys573_state::{name}\s*\(machine_config\s*&config\)', 'drmn4m')
    if not all(n in machine for n in ('k573d(config)', 'casszi(config)', 'KONAMI_573_MULTI_SESSION_UNIT')):
        raise mame.PreparationError('digital/ZI/multi-session composition changed')
    romset, parents, _ = mame._find_romset(source, name)
    sizes = {'maincpu:rom': 0x80000, 'h8_response': 64, 'mcu': 0x8000,
             'cassette:game:eeprom': 140, 'cassette:game:id': 8}
    if {k: r.size for k, r in romset.regions.items()} != sizes:
        raise mame.PreparationError('unexpected digital ROM regions; parent flash dumps must not be installed')
    if len(romset.disks) != 1 or tuple(getattr(romset.disks[0], k) for k in ('name', 'region', 'sha1', 'readonly')) != (
            'a25kaa02', 'runtime', DISK_SHA, True):
        raise mame.PreparationError('unexpected KAA source CD declaration')
    if f'DISK_IMAGE_READONLY( "a25kaa02", 0, BAD_DUMP SHA1({DISK_SHA}) )' not in text:
        raise mame.PreparationError('source CD BAD_DUMP annotation changed')
    undumped = [f for f in romset.files if f.status == 'no_dump']
    if len(undumped) != 1 or (undumped[0].filename, undumped[0].expected_size) != ('hd6473644h.18e', 0x8000):
        raise mame.PreparationError('unexpected undumped H8 firmware')
    index = undumped[0].index
    romset = replace(romset, regions={k: v for k, v in romset.regions.items() if k != 'mcu'},
                     files=[f for f in romset.files if f.index != index],
                     loads=[op for op in romset.loads if op.file_index != index])
    selected = mame._selected_bios(romset, bios)
    expected = {'gka25kaa.u1': ('good', 140, '2b0efc90', '84bfc8dd365e9d51d248d4dbc09274aa3dff345f'),
                'gka25kaa.u6': ('bad_dump', 8, 'ce84419e', '839e8ee080ecfc79021a06417d930e8b32dfc6a1')}
    for spec in romset.files:
        if spec.filename in expected and (spec.status, spec.expected_size, spec.crc32, spec.sha1) != expected[spec.filename]:
            raise mame.PreparationError('source cassette identity/status changed')
    return romset, parents, selected


def source_device_profiles(source):
    expected = {
        'k573dio': {'digital-id.bin': (8, '2b977f4d', '2b108a56653f91cb3351718c45dfcf979bc35ef1')},
        'k573msu': {'flash.20t': (524288, 'b70c65b0', 'd3b2bf9d3f8b1caf70755a0d7fa50ef8bbd758b8'),
                    'm48t58y.6t': (8192, '609ef020', '71b87c8b25b9613b4d4511c53d0a3a3aacf1499d')},
    }
    profiles = {}
    for name, specs in expected.items():
        driver = 'konami/' + name + '.cpp'
        parsed, _, _ = mame.parse_driver((source / 'src/mame' / driver).read_text(), driver)
        romset = parsed[name]
        if {f.filename: (f.expected_size, f.crc32, f.sha1) for f in romset.files} != specs or any(
                f.status != 'good' for f in romset.files) or romset.disks or romset.bios:
            raise mame.PreparationError('daughterboard original ROM declarations changed: ' + name)
        if set(romset.regions) != set(DEVICE_FILES[name]):
            raise mame.PreparationError('daughterboard original regions changed: ' + name)
        profiles[name] = romset
    return profiles


def audit_roms(source, rompath):
    romset, parents, bios = source_profile(source)
    lineage = mame._lineage(SET, parents)
    data, source_files = {}, {}
    for spec in romset.files:
        if spec.bios_id is not None and romset.bios[spec.bios_id] != bios:
            continue
        candidates = [spec.filename]
        if spec.filename == 'gka25kaa.u6':
            candidates.append('gea25aaa.u6') # Historical parent name, identical source size/CRC/SHA1.
        errors = []
        for filename in candidates:
            try:
                one = replace(romset, files=[replace(spec, filename=filename)])
                found, where = mame._load_rom_files(one, rompath, lineage, bios)
                data.update(found)
                source_files[spec.filename] = {'member': filename, 'origin': where[spec.index],
                    'size': spec.expected_size, 'crc32': spec.crc32, 'sha1': spec.sha1, 'status': spec.status}
                break
            except mame.PreparationError as exc:
                errors.append(str(exc))
        else:
            raise mame.PreparationError('; '.join(errors))
    regions = mame.assemble_regions(romset, data, bios)
    if set(regions) != set(FILES) or len(source_files) != 4:
        raise mame.PreparationError('expected original BIOS, H8 response, ZS01 and DS2401')
    payloads = {FILES[k]: v for k, v in regions.items()}
    for name, device in source_device_profiles(source).items():
        data, where = mame._load_rom_files(device, rompath, [], None)
        regions = mame.assemble_regions(device, data, None)
        payloads.update({DEVICE_FILES[name][k]: v for k, v in regions.items()})
        for spec in device.files:
            source_files[spec.filename] = {'device': name, 'member': spec.filename, 'origin': where[spec.index],
                'size': spec.expected_size, 'crc32': spec.crc32, 'sha1': spec.sha1, 'status': spec.status}
    report = {'format': 'pcsx-aow-system573-digital-media-audit-v1', 'set': SET,
              'mame_source_commit': mame.PINNED_MAME_COMMIT, 'parent_chain': lineage,
              'board_execution_verified': False, 'native_profile_available': False,
              'rom_chips_verified': True, 'complete_media_verified': False,
              'cassette': 'ZI: ZS01 and DS2401', 'source_files': source_files,
              'omitted_media': [{'filename': 'hd6473644h.18e', 'status': 'NO_DUMP'}],
              'source_defaults': {'flash': 'eight erased Fujitsu 29F016A chips; no current set dump',
                                  'timekeeper': 'FF SRAM and live source RTC counters'},
              'multi_session_unit': 'Original ROMs audited; pinned MAME device declares no processor or runtime map',
              'disk': {'name': 'a25kaa02.chd', 'source_sha1': DISK_SHA,
                       'status': 'BAD_DUMP', 'verified': False}}
    return payloads, report


def audit(source, rompath, roms_only=False):
    payloads, report = audit_roms(source, rompath)
    if not roms_only:
        candidates = [rompath / n / 'a25kaa02.chd' for n in (SET, 'pcnfrk4m')]
        disk = next((p for p in candidates if p.is_file()), None)
        if disk is None:
            raise mame.PreparationError('original KAA CD a25kaa02.chd is missing; ROM audit alone cannot validate this board')
        tool = shutil.which('chdman')
        if not tool:
            raise mame.PreparationError('chdman required for full original CD verification')
        info = subprocess.check_output([tool, 'info', '-i', str(disk), '-v'], text=True)
        sha = re.search(r'^SHA1:\s*(\S+)', info, re.M)
        if not sha or sha[1] != DISK_SHA or not any(t in info for t in ("Tag='CHTR'", "Tag='CHT2'")):
            raise mame.PreparationError('KAA source CD SHA1/metadata mismatch')
        verify = subprocess.check_output([tool, 'verify', '-i', str(disk)], text=True)
        report['disk'].update(verified=True, origin=str(disk.resolve()), info=info, verification=verify)
        report['complete_media_verified'] = True
    return payloads, report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame-source', type=Path, required=True)
    parser.add_argument('--rompath', type=Path, required=True)
    parser.add_argument('--roms-only', action='store_true', help='explicitly incomplete media audit without a CD')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        payloads, report = audit(args.mame_source, args.rompath, args.roms_only)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, content in payloads.items():
            (args.output_dir / name).write_bytes(content)
        (args.output_dir / 'audit.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Original KAA chips verified; ' + ('CD not audited (explicit ROM-only mode)' if args.roms_only else 'full CD verified'))
        print('Media audit only; native execution remains unverified')
    except (mame.PreparationError, subprocess.CalledProcessError) as exc:
        parser.exit(1, str(exc) + '\n')
