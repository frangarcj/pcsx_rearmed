#!/usr/bin/env python3
"""Execute generated MIPS/Z80 media through the native Capcom ZN adapter.

The coefficients, CAT702 keys, BIOS and game images below are synthetic test
data. They exercise host wiring and never stand in for audited game media.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_capcom_zn_board as prepare
from check_system10_harness import _addiu, _i, _j, _jalr, _lui, _lw, _ori, _sb, _sh, _sw

PROFILES = {p["set"]: p for p in json.loads(
    (ROOT / "scripts/capcom_zn_boards.json").read_text())["profiles"]}


def program() -> bytes:
    code = [_lui(8, 0x9f00), _ori(8, 8, 0x100), _jalr(8), 0]
    # Select the final physically populated 4 MiB bank and execute its helper.
    code += [_lui(8, 0x1fb0), _addiu(9, 0, 7), _sb(9, 8, 0),
             _lui(8, 0x9f40), _ori(8, 8, 0x100), _jalr(8), 0]
    code += [_lui(8, 0x1fb8), _lw(18, 8, 0), 0]
    # Prove that upper RAM is independent of the lower 2 MiB.
    code += [_lui(8, 0x8021), _lui(9, 0x51a7), _ori(9, 9, 0xcafe),
             _sw(9, 8, 0), _lw(20, 8, 0), 0,
             _lui(8, 0x8001), _lw(21, 8, 0), 0]
    # EEPROM programming completes after the real emulated 200 us deadline.
    code += [_lui(8, 0x1faf), _addiu(9, 0, 0x5a), _sb(9, 8, 0)]
    # Select only the ZN MCU: CAT702s remain deselected. Its source digital
    # header is 0x1f (one data byte, default DSW low nibble 0xf).
    code += [_lui(8, 0x1fa1), _addiu(9, 0, 0x8c), _sb(9, 8, 0x300)]
    # A bounded guest delay lets both EEPROM and MCU deadlines elapse and
    # lets the Z80 initialize its stack/QSound before the latch NMI.
    code += [_addiu(9, 0, 10000)]
    delay = len(code)
    code += [_addiu(9, 9, -1), _i(5, 9, 0, -2), 0]
    assert delay + 3 == len(code)
    code += [_lui(8, 0x1faf), _i(0x24, 8, 23, 0), 0]
    # SIO0 mode=1, baud=100 cycles, TX/RX interrupt enable, TX enabled.
    code += [_lui(8, 0x1f80), _addiu(9, 0, 1), _sh(9, 8, 0x1048),
             _addiu(9, 0, 100), _sh(9, 8, 0x104e),
             _addiu(9, 0, 0x0c01), _sh(9, 8, 0x104a), _sb(0, 8, 0x1040)]
    poll = len(code)
    code += [_lw(9, 8, 0x1044), 0, _i(0x0c, 9, 9, 2),
             _i(4, 9, 0, poll - (len(code) + 4)), 0]
    code += [_i(0x24, 8, 22, 0x1040), 0,
             _lw(24, 8, 0x1070), 0]
    # I_STAT acknowledges the IRQ independently of the SIO's retained IRQ
    # status bit. Merely synchronizing the devices must not relatch it.
    code += [_addiu(25, 0, 0xa5), _sw(0, 8, 0x1070), _lw(25, 8, 0x1070), 0]
    code += [_lui(8, 0x1fb6), _addiu(9, 0, 0x40), _sb(9, 8, 0)]
    # Guest GPU fill/readback goes through the actual NEON renderer.
    code += [_lui(8, 0xbf80), _ori(8, 8, 0x1810),
             _lui(9, 0x0200), _ori(9, 9, 0xff), _sw(9, 8, 0),
             _sw(0, 8, 0), _lui(9, 1), _ori(9, 9, 16), _sw(9, 8, 0),
             _lw(9, 8, 4), 0, _lui(9, 0xc000), _sw(9, 8, 0),
             _sw(0, 8, 0), _lui(9, 1), _ori(9, 9, 2), _sw(9, 8, 0),
             _lw(19, 8, 0), 0]
    # Sample input twice through the byte and 32-bit daughterboard maps.
    code += [_lui(8, 0x1fa0), _lui(9, 0x1fb4)]
    loop = 0xbfc00000 + len(code) * 4
    code += [_i(0x24, 8, 14, 0), 0, _lw(15, 9, 0x10), 0, _j(loop), 0]
    return struct.pack(f"<{len(code)}I", *code)


def sound_program() -> bytes:
    rom = bytearray(0x40000)
    rom[:3] = b"\xc3\x00\x01"
    code = bytearray(b"\x31\xfe\xff")

    def poll() -> None:
        code.extend(b"\x3a\x07\xd0\xe6\x80\x28\xf9")

    def command(address: int, value: int) -> None:
        for data, register in ((value >> 8, 0xd000), (value & 255, 0xd001), (address, 0xd002)):
            code.extend(bytes((0x3e, data, 0x32, register & 255, register >> 8)))
        poll()

    poll()
    # Set pitch and loop boundaries before resetting the voice address. The
    # source runs stream frames between commands, so an earlier address write
    # can drift while the default pitch/end registers are still active.
    for address, value in ((0xdf, 0), (0xe1, 0), (0xe2, 1), (0x78, 0x8000),
                           (2, 0), (4, 2), (5, 2), (1, 0), (6, 0x4000)):
        command(address, value)
    code.extend(b"\x76\x18\xfd")
    rom[0x100:0x100 + len(code)] = code
    nmi = bytes.fromhex("f5 db 00 32 10 f0 32 00 d0 af 32 01 d0 3e 06 32 02 d0 f1 ed 45")
    rom[0x66:0x66 + len(nmi)] = nmi
    return bytes(rom)


def media(directory: Path, set_name: str) -> None:
    profile = PROFILES[set_name]
    regions = {r["tag"]: bytearray(r["size"]) for r in profile["rom_regions"]
               if r["tag"] in prepare.OUTPUTS}
    image = program()
    regions["maincpu:rom"][:len(image)] = image
    for offset, register, value in ((0x100, 16, 0x31), (0x2000100, 17, 0x77)):
        helper = struct.pack("<4I", _addiu(register, 0, value), 0x03e00008, 0, 0)
        regions["bankedroms"][offset:offset + len(helper)] = helper
    regions["countryrom"][:4] = bytes.fromhex("44332211")
    regions["audiocpu"][:] = sound_program()
    regions["qsound"][:2] = bytes((0x40, 0x80))
    coefficients = [0] * 0x1000
    coefficients[0x120], coefficients[0x120 + 196] = 0xc000, 0xe000
    prepare.package_regions(set_name, profile, {k: bytes(v) for k, v in regions.items()},
                            struct.pack("<4096H", *coefficients), directory,
                            {"synthetic_fixture": True, "rom_source": "generated MIPS/Z80",
                             "selected_bios": profile.get("default_bios")})


def run(harness: Path, directory: Path, *args: str) -> subprocess.CompletedProcess:
    result = subprocess.run([str(harness), "--capcom-zn", str(directory), *args],
                            capture_output=True, text=True, timeout=120)
    print(result.stderr, end="")
    return result


def check_guest(result: subprocess.CompletedProcess, trace: Path, audio: Path, set_name: str) -> None:
    result.check_returncode()
    with trace.open() as file:
        rows = list(csv.DictReader(file))
    if not rows or rows[0]["pc"] != "bfc00000":
        raise RuntimeError("guest did not start at the BIOS reset vector")
    completed = [row for row in rows if row["r19"] == "001f001f"]
    if not completed:
        raise RuntimeError("guest did not complete the GPU verification stage")
    for reg, value in ((16, 0x31), (17, 0x77), (18, 0x11223344), (19, 0x001f001f),
                       (20, 0x51a7cafe), (21, 0), (22, 0x1f), (23, 0x5a), (25, 0)):
        if not any(row[f"r{reg}"] == f"{value:08x}" for row in completed):
            raise RuntimeError(f"MIPS verification r{reg}={value:08x} missing")
    if not any(int(row["r24"], 16) & 0x80 for row in rows):
        raise RuntimeError("SIO0 RX completion did not set PSX IRQ7")
    for reg, values in ((14, (0xef, 0xff)), (15, (0xfffffffe, 0xffffffff))):
        if any(not any(row[f"r{reg}"] == f"{value:08x}" for row in rows) for value in values):
            raise RuntimeError("guest did not observe input press/release")
    # Independently derive elapsed time from main-CPU cycles. ZN-2 runs at
    # 50 MHz while its SIO remains at 33.8688 MHz and its Z80 at 8 MHz.
    log = result.stdout + result.stderr
    cycles = int(re.search(r"host=.*? cycles=(\d+)", log)[1])
    clock = PROFILES[set_name]["cpu_clock_hz"] // 2
    expected_sio = cycles * 33868800 // clock
    timing = re.search(r"time_us=(\d+) SIO0=(\d+) SIO1=(\d+)", log)
    if tuple(map(int, timing.groups())) != (expected_sio * 1000000 // 33868800,
                                            expected_sio, expected_sio):
        raise RuntimeError("SIO/MCU time did not follow the source clock")
    z80_cycles = int(re.search(r"Z80 cycles=(\d+)", log)[1])
    if z80_cycles != cycles * 8000000 // clock:
        raise RuntimeError("Z80 budget did not follow elapsed main CPU time")
    with wave.open(str(audio)) as wav:
        if wav.getnchannels() != 2 or wav.getsampwidth() != 2 or wav.getnframes() < 100:
            raise RuntimeError("invalid QSound stereo capture")
        pcm = wav.readframes(wav.getnframes())
        if not any(pcm):
            raise RuntimeError("executed Z80 commands produced no PCM")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    harness = parser.parse_args().harness.resolve()
    with tempfile.TemporaryDirectory(prefix="pcsx-capcom-zn-") as temporary:
        root = Path(temporary)
        inputs = root / "inputs.csv"
        inputs.write_text("frame,control,pressed\n0,p1button1,1\n0,p1button4,1\n"
                          "1,p1button1,0\n1,p1button4,0\n")
        for set_name in ("ts2", "sfex", "sfex2"):
            directory = root / set_name
            media(directory, set_name)
            trace, audio = directory / "trace.csv", directory / "audio.wav"
            result = run(harness, directory, "--board-test", "--frames", "3", "--inputs", str(inputs),
                         "--trace", str(trace), "--trace-blocks", "200000", "--audio", str(audio),
                         "--output", str(directory / "frame.ppm"))
            check_guest(result, trace, audio, set_name)
        # KSEG0 bank helpers exercise the actual instruction cache refill.
        # Persistent EEPROM carries the full ID block through programming.
        directory = root / "sfex"
        trace, audio, nvram = directory / "cache.csv", directory / "cache.wav", root / "eeprom.bin"
        initial = bytes([0xff]) * 2048 + bytes(range(32))
        nvram.write_bytes(initial)
        result = run(harness, directory, "--board-test", "--icache", "--frames", "3",
                     "--inputs", str(inputs), "--trace", str(trace), "--trace-blocks", "200000",
                     "--audio", str(audio), "--nvram", str(nvram), "--output", str(directory / "cache.ppm"))
        check_guest(result, trace, audio, "sfex")
        if nvram.read_bytes() != b"\x5a" + initial[1:]:
            raise RuntimeError("persistent AT28C16 data/ID did not preserve exact contents")
        valid = root / "sfex2"
        for name, change in (
            ("unknown", lambda s: s.replace("mame_set=sfex2\n", "mame_set=unknown\n")),
            ("duplicate", lambda s: s + "mame_set=sfex2\n"),
            ("geometry", lambda s: s.replace("vram_size=2097152", "vram_size=1048576")),
        ):
            directory = root / name
            shutil.copytree(valid, directory)
            (directory / "board.ini").write_text(change((valid / "board.ini").read_text()))
            trace = directory / "rejected.csv"
            result = run(harness, directory, "--frames", "1", "--trace", str(trace))
            if result.returncode == 0 or (trace.exists() and len(trace.read_text().splitlines()) > 1):
                raise RuntimeError("invalid Capcom profile reached guest execution")
    print("Capcom ZN synthetic MIPS banks, SIO/MCU/IRQ, EEPROM, input, GPU and QSound PASS")


if __name__ == "__main__":
    main()
