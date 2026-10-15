#!/usr/bin/env python3
"""Inspect retained original-game CSV observations without recreating captures."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path


def rows(path):
    with path.open() as source:
        result = list(csv.DictReader(source))
    assert result and all(None not in row for row in result), path
    return result


def music(data):
    return [row for row in data if row['s573_start'] == '00000000'
            and row['s573_end'] == '00187014' and int(row['s573_mpeg_frames'])]


def source_log(path, native=False):
    text = path.read_text()
    assert all(message not in text for message in
               ('LUA ERROR', 'Fatal error', 'failed', 'execution limit reached')), path
    if native:
        assert 'cpu=interpreter frames=13800' in text
        assert 'Arcade inputs: consumed=1495/1495' in text
        for prefix in ('System573:', 'System573 digital:', 'System573 CD:',
                       'System573 speaker:'):
            line = next(line for line in text.splitlines() if line.startswith(prefix))
            assert 'faults=0' in line, line
    else:
        assert 'SONG_FINAL\t1042\t' in text and 'SOURCE_LAYOUT\t' in text
        assert sum(line.startswith('SONG_INPUT\t') for line in text.splitlines()) == 1476


def verify(root, manifest):
    data = {}
    for gpu in ('neon', 'gl'):
        source_log(root/f'mpeg-{gpu}.log', native=True)
        data[gpu] = rows(root/f'mpeg-{gpu}.csv')
        assert int(data[gpu][-1]['ram_803493f8'], 16) == 1042
        assert int(data[gpu][-1]['ram_80349538'], 16) == 1150
    keys = [key for key in data['neon'][0] if key.startswith('s573_')]
    assert len(keys) == 8 and len(data['neon']) == len(data['gl'])
    assert all(a[key] == b[key] for a, b in zip(data['neon'], data['gl']) for key in keys)

    ref = root/'mame-mpeg-observed'
    source_log(ref/'run.log')
    data['reference'] = rows(ref/'score.csv')
    last = data['reference'][-1]
    assert (int(last['boot_rows']), int(last['pad_rows'])) == (19, 1476)
    assert int(last['ram_803493f8'], 16) == 1042
    for key in ('ram_80349414', 'ram_80349424', 'ram_80349428', 'ram_8034942c'):
        assert last[key] == data['neon'][-1][key], key
    native, reference = music(data['neon']), music(data['reference'])
    cursors = []
    for stream in (native, reference):
        mapping = defaultdict(set)
        for row in stream:
            assert row['s573_fpga'] == 'e000' and row['s573_mpeg_state'] == '4000'
            mapping[int(row['s573_mpeg_frames'])].add(row['s573_current'])
        assert all(len(positions) == 1 for positions in mapping.values())
        cursors.append(mapping)
    shared = sorted(cursors[0].keys() & cursors[1].keys())
    assert shared == list(range(1, 600)), (len(shared), shared[-1])
    assert all(cursors[0][frame] == cursors[1][frame] for frame in shared)

    by_tick = {int(row['ram_803493f8'], 16): row for row in reference}
    phases = defaultdict(list)
    for row in native:
        tick = int(row['ram_803493f8'], 16)
        if tick < 300 or tick not in by_tick:
            continue
        # Keep the two observed callback phases separate; do not rewrite data
        # or manufacture clock/score equality by adjusting their samples.
        phase = int(row['frame']) - tick
        assert phase in (12757, 12758), phase
        phases[phase].append(int(by_tick[tick]['s573_latched_samples'])
                             - int(row['s573_latched_samples']))
    assert sum(map(len, phases.values())) >= 700
    result = {
        'scope': 'retained CSV/log inspection; no emulator or playback rerun',
        'native_status_rows_per_renderer': len(data['neon']),
        'native_gl_neon_status_rows_equal': True,
        'shared_mpeg_frames_with_identical_source_byte_cursor': len(shared),
        'native_first_observed_sync_song_tick': int(native[0]['ram_803493f8'], 16),
        'reference_first_observed_sync_song_tick': int(reference[0]['ram_803493f8'], 16),
        'reference_minus_native_latched_samples_by_native_callback_phase': {
            str(phase): {'count': len(values), 'minimum': min(values), 'maximum': max(values)}
            for phase, values in sorted(phases.items())},
        'raw_latched_sample_delta_histogram': dict(sorted(Counter(
            value for values in phases.values() for value in values).items())),
        'scores': {'native': 1150, 'reference_song_tick_aligned': int(last['ram_80349538'], 16)},
        'judgement_equivalence': 'OPEN',
    }
    for label, name in (('reference_pads_four_ticks_earlier', 'mame-mpeg-shift'),
                        ('reference_pads_three_ticks_earlier', 'mame-mpeg-nearest')):
        source_log(root/name/'run.log')
        final = rows(root/name/'score.csv')[-1]
        assert int(final['ram_803493f8'], 16) == 1042
        assert (int(final['boot_rows']), int(final['pad_rows'])) == (19, 1476)
        result['scores'][label] = int(final['ram_80349538'], 16)

    # These are hashes recorded before deletion, not a fresh PCM comparison.
    archived = {item['path']: item for item in
                (json.loads(line) for line in manifest.read_text().splitlines())
                if item['action'] == 'removed_generated_capture'}
    historical_audio = {}
    for suffix in ('spu.wav', 'speaker.wav'):
        paths = [root/f'{name}-{suffix}' for name in
                 ('mpeg-neon', 'mpeg-gl', 'player-pads-neon')]
        entries = [archived[str(path.resolve())] for path in paths]
        hashes = {entry['sha256'] for entry in entries}
        assert len(hashes) == 1
        historical_audio[suffix] = {
            'sha256_recorded_before_cleanup': hashes.pop(),
            'raw_captures_present': [path.exists() for path in paths],
            'fresh_audio_verification': False}
    result['historical_audio'] = historical_audio
    result['csv_sha256'] = {name: hashlib.sha256((root/name).read_bytes()).hexdigest()
                            for name in ('mpeg-neon.csv', 'mpeg-gl.csv',
                                         'mame-mpeg-observed/score.csv')}
    return result


def verify_pad_reads(root):
    """Check navigation isolation and actual source pad polls, without rerunning."""
    baseline = rows(root/'mame-mpeg-observed/score.csv')
    control_root, observed_root = root/'mame-menu-control', root/'mame-pad-reads'
    for path in (control_root, observed_root):
        source_log(path/'run.log')
        log = (path/'run.log').read_text()
        assert sum(line.startswith('BOOT_CANCELLED\t1\t') for line in log.splitlines()) == 4
    control = rows(control_root/'score.csv')
    observed = rows(observed_root/'score.csv')
    assert len(baseline) == len(control) == len(observed)
    changed_start_rows = 0
    for before, after, watched in zip(baseline, control, observed):
        assert after == watched, 'passive read tap must preserve frame observations'
        # Cancellation accounts for future scheduled retries immediately;
        # the original replay consumes those rows later at their old times.
        applied, cancelled = int(after['boot_rows']), int(after['cancelled_boot_rows'])
        assert applied <= int(before['boot_rows']) <= applied+cancelled
        for key in before:
            if key not in ('boot_rows', 's573_pad_bits'):
                assert before[key] == after[key], key
        if before['s573_pad_bits'] != after['s573_pad_bits']:
            assert int(before['s573_pad_bits'], 16) ^ int(after['s573_pad_bits'], 16) == 0x8000
            changed_start_rows += 1
        if int(after['ram_803493f8'], 16):
            assert int(after['s573_pad_bits'], 16) & 0x8000, 'Start held during song'
    final = observed[-1]
    assert (int(final['boot_rows']), int(final['cancelled_boot_rows']), int(final['pad_rows'])) == (15, 4, 1476)
    assert int(final['ram_80349538'], 16) == 25950
    polls = rows(observed_root/'reads.csv')
    assert 0 < len(polls) <= 100000
    assert f'PAD_READS\t{len(polls)}' in (observed_root/'run.log').read_text()
    for poll in polls:
        assert int(poll['address'], 16) == 0x1f400008
        assert 1 <= int(poll['song_tick']) <= 1042
        assert int(poll['data'], 16) & 0x8000, 'actual pad poll observes Start held'
    metadata = json.loads((observed_root/'observation.json').read_text())
    for name, recorded in metadata['files'].items():
        assert hashlib.sha256((observed_root/name).read_bytes()).hexdigest() == recorded['sha256']
    return {'applied_navigation_rows': 15, 'cancelled_redundant_start_rows': 4,
            'unchanged_pad_transitions': 1476, 'changed_start_snapshot_rows': changed_start_rows,
            'reference_score_with_and_without_redundant_start': 25950,
            'passive_tap_preserves_all_frame_observations': True,
            'actual_pad_reads': len(polls),
            'actual_read_pc_counts': dict(Counter(poll['pc'] for poll in polls)),
            'actual_read_mask_counts': dict(Counter(poll['mask'] for poll in polls)),
            'native_actual_pad_reads_observed': False, 'judgement_equivalence': 'OPEN'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-dir', required=True, type=Path)
    parser.add_argument('--cleanup-manifest', type=Path)
    parser.add_argument('--pad-reads', action='store_true',
                        help='also inspect isolated navigation and passive MAME pad polls')
    args = parser.parse_args()
    root = args.evidence_dir.resolve()
    manifest = args.cleanup_manifest or root.parents[2]/'disk-cleanup-20261015.jsonl'
    result = verify(root, manifest)
    if args.pad_reads:
        result['pad_reads'] = verify_pad_reads(root)
    print(json.dumps(result, indent=2))
    print('Retained MPEG observations verified; judgement equivalence remains OPEN')


if __name__ == '__main__':
    main()
