#!/usr/bin/env python3
"""Inspect retained original Plus print/player evidence without regenerating WAVs."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

from PIL import Image


def line(log, prefix):
    return next(value for value in log.splitlines() if value.startswith(prefix))


def page(path):
    with Image.open(path) as source:
        image = source.convert('RGB')
    assert image.size == (800, 600), path
    counts, hashes = [], []
    for cell in range(16):
        x, y = 81+(cell % 4)*160, 80+(cell // 4)*111
        rgb = image.crop((x, y, x+154, y+106)).tobytes()
        counts.append(sum(rgb[i:i+3] != b'\xff\xff\xff' for i in range(0, len(rgb), 3)))
        hashes.append(hashlib.sha256(rgb).hexdigest())
    assert min(counts) > 1000, counts
    # The original TEST PRINT has two logo cells and fourteen repeated scenes.
    assert len(set(hashes[1:15])) == 1
    assert hashes[0] != hashes[1] and hashes[15] != hashes[1]
    return {'rgb_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
            'nonwhite_pixels_per_cell': counts}


def verify(root):
    comparison = json.loads((root/'operator-comparison.json').read_text())
    result = {'scope': 'retained logs, PNGs and hashes; no emulator or audio rerun', 'pages': {}}
    for gpu in ('neon', 'gl'):
        log = (root/f'operator-{gpu}.log').read_text()
        assert 'cpu=interpreter frames=6000' in log and 'Arcade inputs: consumed=10/10' in log
        assert 'failed' not in log and 'execution limit reached' not in log
        assert 'profile=tmoshsp' in line(log, 'SystemGV: profile=')
        io = line(log, 'Tokimeki IO:')
        assert all(token in io for token in ('packets=60 rejected=0', 'captures=18 prints=1', 'unknown=40 faults=0')), io
        assert 'faults=00000000/00000000' in line(log, 'SystemGV: NCR=')
        result['pages'][gpu] = page(root/f'prints-{gpu}/print-0001.png')
        assert result['pages'][gpu]['rgb_sha256'] == comparison['original_operator_test'][gpu]['page_rgb_sha256']
        # Persisted EEPROM remains available for direct verification.
        nv = root/f'operator-{gpu}.nv'
        recorded = comparison['original_operator_test'][gpu][nv.name]
        assert nv.stat().st_size == 128
        assert hashlib.sha256(nv.read_bytes()).hexdigest() == recorded['sha256']
    hashes = {}
    for suffix, separator in (('spu.wav', '-'), ('speaker.wav', '-'), ('ram', '.'), ('nv', '.')):
        values = [comparison['original_operator_test'][gpu][f'operator-{gpu}{separator}{suffix}']['sha256']
                  for gpu in ('neon', 'gl')]
        assert values[0] == values[1], suffix
        hashes[suffix] = values[0]
    result['recorded_operator_gl_neon_equal_hashes'] = hashes

    log = (root/'player-gl.log').read_text()
    assert 'cpu=interpreter frames=9000' in log and 'Arcade inputs: consumed=98/98' in log
    assert 'failed' not in log and 'execution limit reached' not in log
    assert 'profile=tmoshsp' in line(log, 'SystemGV: profile=')
    assert 'faults=00000000/00000000' in line(log, 'SystemGV: NCR=')
    assert 'rejected=0 captures=0 prints=0 unknown=40 faults=0' in line(log, 'Tokimeki IO:')
    cdda = line(log, 'SystemGV CDDA:')
    assert int(re.search(r'nonzero_frames=(\d+)', cdda)[1]) > 0
    speaker = line(log, 'SystemGV speaker:')
    assert 'SPU_gain=0.75 SPU_channels=swapped CDDA_gain=1' in speaker and 'faults=0' in speaker
    assert int(re.search(r'SPU_nonzero=(\d+)', speaker)[1]) > 0
    observed = json.loads((root/'player-observation.json').read_text())
    shot = root/'player-gl.png'
    assert hashlib.sha256(shot.read_bytes()).hexdigest() == observed['files'][shot.name]['sha256']
    result['player'] = {'frames': 9000, 'events': 98, 'scene': observed['scene'],
                        'cdda_nonzero_frames': int(re.search(r'nonzero_frames=(\d+)', cdda)[1]),
                        'printed_final_report': False}
    reference = (root/'mame-reference/run.log').read_text()
    assert 'PLUS_OPERATOR_REFERENCE\t10\t10' in reference and 'LUA ERROR' not in reference
    result['pages']['mame'] = page(root/'mame-reference/page.png')
    result['pages_pixel_identical'] = len({value['rgb_sha256'] for value in result['pages'].values()}) == 1
    result['fresh_audio_verification'] = False
    result['raw_capture_files_present'] = sorted(str(p.relative_to(root)) for p in root.rglob('*')
                                                if p.suffix in ('.wav', '.ram', '.ppm'))
    assert not result['raw_capture_files_present'], 'completed batch must release raw captures'
    return result


def verify_player_sheet(root):
    """Check the separate original player print; never infer it from TEST PRINT."""
    with (root/'extended-player.csv').open() as source:
        events = list(csv.DictReader(source))
    with (root/'player.csv').open() as source:
        short_events = list(csv.DictReader(source))
    assert len(events) == 318 and events[:len(short_events)] == short_events
    assert {row['control'] for row in events} == {'coin1', 'p1button1', 'gsr', 'heartbeat'}
    assert all(row['control'] == 'p1button1' for row in events[len(short_events):])
    assert [int(row['frame']) for row in events] == sorted(int(row['frame']) for row in events)
    observed = json.loads((root/'extended-observation.json').read_text())
    for name, recorded in observed['files'].items():
        assert hashlib.sha256((root/name).read_bytes()).hexdigest() == recorded['sha256'], name
    pages = {}
    for gpu in ('gl', 'neon'):
        log = (root/f'extended-{gpu}.log').read_text()
        assert 'cpu=interpreter frames=20000' in log and 'Arcade inputs: consumed=318/318' in log
        assert 'failed' not in log and 'execution limit reached' not in log
        assert 'profile=tmoshsp' in line(log, 'SystemGV: profile=')
        assert 'faults=00000000/00000000' in line(log, 'SystemGV: NCR=')
        assert 'packets=58 rejected=0 captures=16 prints=1 unknown=40 faults=0' in line(log, 'Tokimeki IO:')
        directory = 'prints-extended' if gpu == 'gl' else 'prints-extended-neon'
        pages[gpu] = page(root/directory/'print-0001.png')
        assert pages[gpu]['rgb_sha256'] != page(root/f'prints-{gpu}/print-0001.png')['rgb_sha256']
    ram_gl = observed['removed_raw_captures']['extended-gl.ram']['sha256']
    ram_neon = json.loads((root/'extended-neon-pruned.json').read_text())['extended-neon.ram']['sha256']
    assert ram_gl == ram_neon
    assert (root/'extended-gl.nv').read_bytes() == (root/'extended-neon.nv').read_bytes()
    reference = (root/'mame-player-reference/run.log').read_text()
    assert 'PLUS_PLAYER_REFERENCE\t318\t318' in reference and 'LUA ERROR' not in reference
    pages['mame'] = page(root/'mame-player-reference/page.png')
    assert pages['mame']['rgb_sha256'] != page(root/'mame-reference/page.png')['rgb_sha256']
    return {'frames': 20000, 'events': len(events), 'test_or_service_inputs': False,
            'player_sticker_sheet_printed': True, 'captures': 16, 'prints': 1,
            'scene': observed['scene'], 'recorded_gl_neon_ram_sha256': ram_gl,
            'awarded_card_visual_observation': observed['awarded_card_visual_observation'],
            'reference_scope': observed['reference_scope'],
            'reference_result_equivalence_verified': False,
            'pages': pages, 'fresh_audio_verification': False,
            'physical_printing_verified': False}


def verify_player_timing(root):
    timing = root/'timing'
    summary = json.loads((timing/'summary.json').read_text())
    observation = json.loads((timing/'observation.json').read_text())
    assert summary['frequency'] == 33868800 and summary['events'] == 318
    assert summary['snapshots_pruned'] and not (timing/'native.csv').exists()
    assert 0 < summary['maximum_bound_width_seconds'] < 0.008
    for path, key in ((root/'extended-player.csv', 'inputs_sha256'),
                      (timing/'input-bounds.csv', 'bounds_sha256')):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == summary[key]
    with (root/'extended-player.csv').open() as source:
        inputs = list(csv.DictReader(source))
    with (timing/'input-bounds.csv').open() as source:
        bounds = list(csv.DictReader(source))
    with (timing/'mame-midpoint/applied.csv').open() as source:
        applied = list(csv.DictReader(source))
    assert len(inputs) == len(bounds) == len(applied) == 318
    for index, (event, bound, actual) in enumerate(zip(inputs, bounds, applied), 1):
        assert all(event[key] == bound[key] for key in ('frame', 'control', 'pressed'))
        assert int(bound['event']) == int(actual['event']) == index
        assert actual['frame'] == event['frame'] and actual['control'] == event['control']
        assert actual['value'] == event['pressed']
        lo, hi = int(bound['lower_cycles']), int(bound['upper_cycles'])
        assert 0 <= hi-lo < 0.008*summary['frequency']
        midpoint = (lo+hi)/(2*summary['frequency'])
        assert abs(float(actual['target_seconds'])-midpoint) < 1e-10
        assert abs(float(actual['actual_seconds'])-midpoint) < 1e-9
        assert abs(float(actual['error_ms'])) < 1e-6
    native = (timing/'native.log').read_text()
    assert 'frames=20000 cycles=11173961591 blocks=1045900507 pc=800da374' in native
    assert 'Arcade inputs: consumed=318/318' in native
    assert 'faults=00000000/00000000' in line(native, 'SystemGV: NCR=')
    assert (timing/'native.png').read_bytes() == (root/'extended-neon.png').read_bytes()
    assert (timing/'native.nv').read_bytes() == (root/'extended-neon.nv').read_bytes()
    reference = (timing/'mame-midpoint/run.log').read_text()
    assert 'PLUS_CYCLE_REFERENCE\t318\t318' in reference and 'LUA ERROR' not in reference
    for name, recorded in observation['files'].items():
        assert hashlib.sha256((timing/name).read_bytes()).hexdigest() == recorded['sha256']
    return {'events': len(bounds), 'source_clock_hz': summary['frequency'],
            'maximum_input_time_interval_seconds': summary['maximum_bound_width_seconds'],
            'old_60hz_first_error_ms': float(bounds[0]['old_60hz_minus_midpoint_ms']),
            'old_60hz_last_error_ms': float(bounds[-1]['old_60hz_minus_midpoint_ms']),
            'mame_page': page(timing/'mame-midpoint/page.png'),
            'result_observation': observation['result_observation'],
            'exact_native_input_timestamps_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-dir', required=True, type=Path)
    parser.add_argument('--with-player-sheet', action='store_true',
                        help='also inspect the extended player print and its MAME reference')
    parser.add_argument('--with-player-timing', action='store_true',
                        help='also inspect bounded cycle timing and the MAME timer replay')
    args = parser.parse_args()
    root = args.evidence_dir.resolve()
    result = verify(root)
    if args.with_player_sheet or args.with_player_timing:
        result['extended_player_sheet'] = verify_player_sheet(root)
    if args.with_player_timing:
        result['player_timing'] = verify_player_timing(root)
    print(json.dumps(result, indent=2))
    print('Original Plus player sticker output verified; MAME result equivalence and physical printing remain OPEN'
          if args.with_player_sheet or args.with_player_timing else
          'Original Plus operator grid and bounded player observations verified; extended player print not checked')


if __name__ == '__main__':
    main()
