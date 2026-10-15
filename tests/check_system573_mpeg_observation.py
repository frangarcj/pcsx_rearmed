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


def verify_native_pad_reads(root):
    native_root, control_root = root/'native-pad-reads', root/'native-pad-control'
    ref_root = root/'mame-poll-aligned'
    for path in (native_root, control_root):
        source_log(path/'run.log', native=True)
        state = json.loads((path/'pruned.json').read_text())
        assert state['end.ram']['bytes'] == 4194304
        assert (state['end.ram']['song_tick'], state['end.ram']['score']) == (1042, 1150)
        assert not (path/'end.ram').exists() and not (path/'frame.ppm').exists()
    assert (native_root/'frame.png').read_bytes() == (control_root/'frame.png').read_bytes()
    source_log(ref_root/'run.log')
    assert 'PAD_TICK_OFFSET\t1' in (ref_root/'run.log').read_text()
    assert int(rows(ref_root/'score.csv')[-1]['ram_80349538'], 16) == 24300
    streams = {}
    for name, path in (('native', native_root), ('original_reference', root/'mame-pad-reads'),
                       ('aligned_reference', ref_root)):
        by_tick = defaultdict(list)
        for row in rows(path/'reads.csv'):
            tick = int(row['ram_803493f8'], 16) if name == 'native' else int(row['song_tick'])
            if 1 <= tick <= 1041:
                by_tick[tick].append(row)
        assert sorted(by_tick) == list(range(1, 1042))
        assert all(len(value) == 2 for value in by_tick.values())
        streams[name] = by_tick
    mismatches, deltas, read_gaps = 0, [], Counter()
    for tick, native in streams['native'].items():
        read_gaps[int(native[1]['cycles'])-int(native[0]['cycles'])] += 1
        for a, old, aligned in zip(native, streams['original_reference'][tick],
                                   streams['aligned_reference'][tick]):
            assert a['address'] == aligned['address'] == '1f400008'
            assert int(a['width']) == 2 and aligned['mask'] == '0000ffff'
            assert int(a['reported_pc'], 16)-4 == int(aligned['pc'], 16) == 0x8001876c
            mismatches += a['data'] != old['data']
            assert a['data'] == aligned['data']
            if tick >= 300:
                deltas.append(int(aligned['latched_samples'])-int(a['s573_latched_samples']))
    assert mismatches == 492
    assert set(deltas) == {2424, 2425, 2426}
    nv = [(path/'state.nv').read_bytes() for path in (native_root, control_root)]
    assert len(nv[0]) == len(nv[1])
    nv_diff = [i for i, (a, b) in enumerate(zip(*nv)) if a != b]
    allowed = set(range(20, 24)) | set(range(0x1002018, 0x1002020))
    assert nv_diff and set(nv_diff) <= allowed, 'NVRAM differences outside CRC/live RTC'
    ram_hashes = {name: json.loads((path/'pruned.json').read_text())['end.ram']['sha256']
                  for name, path in (('observed', native_root), ('control', control_root))}
    metadata = json.loads((native_root/'comparison.json').read_text())
    for name, recorded in metadata['files'].items():
        assert hashlib.sha256((root/name).read_bytes()).hexdigest() == recorded['sha256']
    return {'native_reads_during_song': 2082, 'native_read_pairs': 1041,
            'old_reference_mismatched_reads': mismatches, 'aligned_reference_matching_reads': 2082,
            'native_reported_pc': '80018770', 'source_instruction_pc': '8001876c',
            'native_read_pair_cycle_gap_histogram': dict(read_gaps),
            'reference_minus_native_latched_samples_after_tick300': dict(sorted(Counter(deltas).items())),
            'native_score_observed_and_control': 1150, 'aligned_reference_score': 24300,
            'native_final_images_equal': True, 'nv_differing_offsets': nv_diff,
            'native_final_ram_sha256': ram_hashes,
            'native_full_ram_equivalence_verified': False,
            'fresh_audio_verification': False, 'judgement_equivalence': 'OPEN'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-dir', required=True, type=Path)
    parser.add_argument('--cleanup-manifest', type=Path)
    parser.add_argument('--pad-reads', action='store_true',
                        help='also inspect isolated navigation and passive MAME pad polls')
    parser.add_argument('--native-pad-reads', action='store_true',
                        help='also compare actual native polls with the corrected reference')
    args = parser.parse_args()
    root = args.evidence_dir.resolve()
    manifest = args.cleanup_manifest or root.parents[2]/'disk-cleanup-20261015.jsonl'
    result = verify(root, manifest)
    if args.pad_reads or args.native_pad_reads:
        result['pad_reads'] = verify_pad_reads(root)
    if args.native_pad_reads:
        result['native_pad_reads'] = verify_native_pad_reads(root)
    print(json.dumps(result, indent=2))
    print('Retained MPEG observations verified; judgement equivalence remains OPEN')


if __name__ == '__main__':
    main()
