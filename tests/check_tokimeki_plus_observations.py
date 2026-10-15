#!/usr/bin/env python3
"""Inspect retained original Plus print/player evidence without regenerating WAVs."""
import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-dir', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.evidence_dir.resolve()), indent=2))
    print('Original Plus operator grid and player observations verified; final report remains OPEN')


if __name__ == '__main__':
    main()
