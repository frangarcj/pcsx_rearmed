#!/usr/bin/env python3
"""Compare MAME RTC firmware stores through the production Mac System12 board.

Use the same firmware and RTC epoch, with zero address/value/cycle tolerance.
Repeat across CPU backends and board catch-up cadences. Observer controls must
retain the complete board report and framebuffer. This is a serial RTC fixture,
not whole-game, shared-mailbox scheduler or pixel/audio fidelity validation.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from check_system12_vblank import prepare

RTC_EPOCH = [0x00, 0x00, 0x12, 0x07, 0x04, 0x10, 0x26]


def run_case(harness, directory, words, drc, coarse, vblank_only):
    directory.mkdir()
    prepare(directory, coarse)
    with (directory / 'sub.bin').open('r+b') as stream:
        stream.seek(0x100)
        stream.write(struct.pack('>' + 'H' * len(words), *words))
    common = [str(harness), '--system12', str(directory), '--frames', '1']
    if drc:
        common += ['--drc']
    if vblank_only:
        common += ['--system12-vblank-sync-only']
    command = common + ['--system12-edge-trace', str(directory / 'edges.csv'),
                        '--system12-trace-shared-writes', '--output',
                        str(directory / 'frame.ppm')]
    control_command = common + ['--output', str(directory / 'control.ppm')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    control = subprocess.run(control_command, capture_output=True, text=True, timeout=30)
    if result.returncode or control.returncode:
        raise RuntimeError(result.stderr + control.stderr)
    for log in (result.stderr, control.stderr):
        # MAME's store cycles hold only under the board's source timing; a
        # console-timing harness shifts them by whole bus operations.
        if 'timing=source-experiment' not in log:
            raise RuntimeError('Rebuild the harness with SYSTEM12_TIMING=1 (and DRC=1).\n' + log)
        if 'overflow' in log or 'halted=1' in log:
            raise RuntimeError(log)
        h8 = re.search(r'illegal=([0-9a-f]+):([0-9a-f]+) unsupported=([0-9a-f]+)', log)
        if not h8 or any(int(value, 16) for value in h8.groups()):
            raise RuntimeError(log)
        if 'unmapped reads=0 writes=0' not in log:
            raise RuntimeError(log)
    observed_state = [s for s in result.stderr.splitlines() if s.startswith('System12:')]
    control_state = [s for s in control.stderr.splitlines() if s.startswith('System12:')]
    same = (observed_state == control_state and
            (directory / 'frame.ppm').read_bytes() == (directory / 'control.ppm').read_bytes())
    if not same:
        raise AssertionError(('observer changes execution', observed_state, control_state))
    with (directory / 'edges.csv').open() as stream:
        rows = [{k: v if k == 'event' else int(v) for k, v in row.items()}
                for row in csv.DictReader(stream)]
    assert all(a['sequence'] < b['sequence'] for a, b in zip(rows, rows[1:]))
    stores = [r for r in rows if r['event'] == 'h8_store']
    return dict(cpu='drc' if drc else 'interpreter', coarse=coarse,
                vblank_sync_only=vblank_only, command=command,
                control_command=control_command, observer_control_equal=same,
                stores=stores, stderr=result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--mame', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    harness = args.harness.resolve()
    oracle = json.loads(args.mame.read_text())
    if oracle['schema'] != 'pcsx-aow.mame-system12-rtc.v1' or oracle['clock']['h8_hz'] != 16934400:
        raise ValueError('incompatible MAME evidence schema or H8 clock')
    if len(oracle['runs']) != 1 or oracle['runs'][0]['returncode'] != 0 or oracle['runs'][0]['timed_out']:
        raise ValueError('MAME capture did not exit successfully')
    if oracle['rtc_seed']['seeded_rtc_bytes'] != RTC_EPOCH:
        raise ValueError('MAME RTC epoch differs from production native epoch')
    resets = oracle['reset_events']
    if len(resets) != 1 or resets[0]['time'] != dict(seconds=0, attoseconds=0):
        raise ValueError('comparison requires one reset at time zero')
    words = [int(w, 0) if isinstance(w, str) else w for w in oracle['firmware_words']]
    expected = [(r['address'], r['value'], r['cycle_ceil'], 8)
                for r in oracle['shared_observations']]
    if len(expected) != 8 or [r[0] for r in expected] != list(range(0x080000, 0x080008)):
        raise ValueError('MAME did not capture exactly eight consecutive byte stores')
    hashes = oracle['rom_hashes']
    if (hashes['parent_rom_before'] != hashes['parent_rom_after'] or
            hashes['lua_template_before'] != hashes['lua_template_after']):
        raise ValueError('MAME modified its parent ROM or Lua template')
    captures, differences = [], []
    with tempfile.TemporaryDirectory(prefix='pcsx-rtc-probe-') as tmp:
        root = Path(tmp)
        # An atomic word store must emit exactly one event and retain the two
        # byte-write counter, while the RTC firmware below exercises byte stores.
        word_case = run_case(harness, root / 'word-observer',
                             [0x7900, 0xbeef, 0x6ba0, 0x0008, 0x0040, 0x0180],
                             False, False, False)
        assert [(s['address'], s['value'], s['width']) for s in word_case['stores']] == [
            (0x080040, 0xbeef, 16)], word_case
        assert 'shared r=0 w=2' in word_case['stderr'], word_case
        for drc in (False, True):
            for coarse in (False, True):
                for sync in (False, True):
                    case = run_case(harness, root / f'{int(drc)}-{int(coarse)}-{int(sync)}',
                                    words, drc, coarse, sync)
                    actual = [(r['address'], r['value'], r['h8_cycle'], r['width'])
                              for r in case['stores']]
                    delta = [dict(index=i, mame=want, native=got)
                             for i, (want, got) in enumerate(zip(expected, actual))
                             if want != got]
                    if len(expected) != len(actual):
                        delta.append(dict(mame_count=len(expected), native_count=len(actual)))
                    case['equal'] = not delta
                    case['differences'] = delta
                    captures.append(case)
                    differences.extend(delta)
                    print(f"{case['cpu']} coarse={int(coarse)} vblank_only={int(sync)}: "
                          f"{len(actual)} stores, {len(delta)} differences", flush=True)
    evidence = dict(harness=str(harness),
                    harness_sha256=hashlib.sha256(harness.read_bytes()).hexdigest(),
                    mame_evidence=str(args.mame),
                    mame_evidence_sha256=hashlib.sha256(args.mame.read_bytes()).hexdigest(),
                    firmware_words=words, seeded_rtc_bytes=RTC_EPOCH,
                    compared_observations=len(expected) * len(captures),
                    equal=not differences, captures=captures, word_observer=word_case,
                    scope=__doc__)
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print('System12 production RTC serial comparison ' + ('FAIL' if differences else 'PASS'))
    return int(bool(differences))


if __name__ == '__main__':
    raise SystemExit(main())
