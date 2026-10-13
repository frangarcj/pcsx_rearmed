#!/usr/bin/env python3
"""Instruction-driven smoke tests for the native Namco System 10 adapter.

All media is synthetic. The fixtures use genuine MAME profile geometry and
the real passive word permutations to encode MIPS programs into MEM(M) flash
and MEM(N) NAND boot pages. They validate host integration and DMA, not game
bootability or game-specific decryption.
"""
from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / "scripts"))
import prepare_system10_board as prepare

REGISTRY = json.loads((ROOT / "scripts/system10_boards.json").read_text(encoding="utf-8"))
PROFILES = {row["set"]: row for row in REGISTRY["profiles"]}
MEMM_PROFILE = PROFILES["mrdrilr2"]
MEMN_PROFILE = PROFILES["chocovdr"]
DATA_WORDS = bytes(range(0xa0, 0xb0))
DMA_EXPECTED_WORD = int.from_bytes(DATA_WORDS[:4], "little")

MRDRILR2_PERM = (0xc, 0xd, 0xf, 0xe, 0xb, 0xa, 0x9, 0x8,
                 0x7, 0x6, 0x4, 0x1, 0x2, 0x5, 0x0, 0x3)
CHOCOVDR_PERM = (0xd, 0xc, 0xe, 0xf, 0x9, 0x8, 0xa, 0xb,
                 0x4, 0x7, 0x6, 0x5, 0x0, 0x3, 0x2, 0x1)
DATA_XOR = 0xaaaa


def _i(op: int, rs: int, rt: int, immediate: int) -> int:
    return ((op & 0x3f) << 26) | ((rs & 31) << 21) | ((rt & 31) << 16) | (immediate & 0xffff)


def _lui(rt: int, imm: int) -> int:
    return _i(0x0f, 0, rt, imm)


def _ori(rt: int, rs: int, imm: int) -> int:
    return _i(0x0d, rs, rt, imm)


def _addiu(rt: int, rs: int, imm: int) -> int:
    return _i(0x09, rs, rt, imm)


def _lw(rt: int, base: int, offset: int) -> int:
    return _i(0x23, base, rt, offset)


def _lhu(rt: int, base: int, offset: int) -> int:
    return _i(0x25, base, rt, offset)


def _sw(rt: int, base: int, offset: int) -> int:
    return _i(0x2b, base, rt, offset)


def _sh(rt: int, base: int, offset: int) -> int:
    return _i(0x29, base, rt, offset)


def _sb(rt: int, base: int, offset: int) -> int:
    return _i(0x28, base, rt, offset)


def _j(address: int) -> int:
    return 0x08000000 | ((address >> 2) & 0x03ffffff)


def _jalr(rs: int, rd: int = 31) -> int:
    return ((rs & 31) << 21) | ((rd & 31) << 11) | 0x09


def _plain_to_passive_raw(word: int, permutation: tuple[int, ...]) -> int:
    """Invert the source's data XOR plus output-bit permutation."""
    permuted = 0
    for output_bit, input_bit in enumerate(permutation):
        permuted |= ((word >> (15 - output_bit)) & 1) << input_bit
    return permuted ^ DATA_XOR


def encode_memm_boot(plain: bytes) -> bytes:
    if len(plain) & 1:
        raise ValueError("MEM(M) fixture words must be aligned")
    out = bytearray(len(plain))
    for off in range(0, len(plain), 2):
        word = int.from_bytes(plain[off:off + 2], "little")
        out[off:off + 2] = _plain_to_passive_raw(word, MRDRILR2_PERM).to_bytes(2, "little")
    return bytes(out)


def encode_memn_bytes(plain: bytes) -> bytes:
    if len(plain) & 1:
        raise ValueError("MEM(N) fixture words must be aligned")
    out = bytearray(len(plain))
    for off in range(0, len(plain), 2):
        # MAME reads NAND words high byte first, then stores the passive result
        # to the PSX's little-endian main ROM.
        word = int.from_bytes(plain[off:off + 2], "little")
        raw = _plain_to_passive_raw(word, CHOCOVDR_PERM)
        out[off:off + 2] = raw.to_bytes(2, "big")
    return bytes(out)


def make_program(model: str) -> list[int]:
    """Build a small PSX MIPS program that records actual board transactions."""
    # The shared arcade BIOS polls SIO1 error flags; a console's fixed
    # 0x01a5 status deadlocks it. Exercise real SIO lanes through KSEG1.
    code: list[int] = [_lui(10, 0xbf80), _lhu(25, 10, 0x1054), 0,
        _addiu(11, 0, 0xce), _sb(11, 10, 0x1058),
        _lhu(26, 10, 0x1058), 0,
        _addiu(11, 0, 0x50), _sh(11, 10, 0x105e),
        _lhu(27, 10, 0x105e), 0]
    # Poll the active-low base IN1 port and preserve the last observed word in
    # upper RAM so traces can show both the pressed and released states.
    code += [_lui(8, 0x1fba), _lui(9, 0x8021), _ori(9, 9, 0x0020)]
    if model in ("MEMN", "MEM(N)"):
        # Select NAND0 page 0x0300 with a real 00h read command and its three
        # source-mapped address registers.
        code += [_lui(10, 0x1f41), _sb(0, 10, 0)]
        code += [_lui(10, 0x1f42), _sb(0, 10, 0)]
        code += [_lui(10, 0x1f43), _addiu(11, 0, 0), _sb(11, 10, 0)]
        code += [_lui(10, 0x1f44), _addiu(11, 0, 3), _sb(11, 10, 0)]

        # Start channel 5 NAND->RAM transfer. The board's actual word pipeline
        # emits 16 bytes for BCR 00010004 into 4 MiB upper RAM.
        code += [_lui(10, 0x1f80), _ori(10, 10, 0x10f0),
                 _lui(11, 0x0080), _sw(11, 10, 0)]
        code += [_lui(10, 0x1f80), _ori(10, 10, 0x10d0),
                 _lui(11, 1), _ori(11, 11, 4), _sw(11, 10, 4),
                 _lui(11, 0x21), _sw(11, 10, 0)]
        code += [_lui(11, 0x0100), _ori(11, 11, 0x0200), _sw(11, 10, 8)]
        code += [_lui(12, 0x8021), _lw(13, 12, 0), 0,
                 _addiu(22, 13, 0), _sw(13, 9, 4)]
    else:
        # MEM(M)'s bank predictor switches the read window from Intel flash to
        # the source's passive-unscrambled NAND ROM. Execute a real MIPS helper
        # through the dynamic read/fetch path with JALR.
        code += [_lui(10, 0x1fb4), _sh(0, 10, 0), _lw(0, 10, 0), 0]
        code += [_lui(11, 0x9f40), _ori(11, 11, 0x0400), _jalr(11), 0]
        code += [_lui(12, 0x8021), _sw(17, 12, 4)]

    # Read and acknowledge the source timer interrupt through the real NS10
    # interrupt status/ack registers.
    code += [_lhu(16, 8, 0x1a), 0, _sw(16, 9, 0x28),
             _addiu(17, 0, 8), _sh(17, 8, 0x18),
             _lhu(18, 8, 0x1a), 0, _sw(18, 9, 0x2c)]

    # Upper PSX RAM is real storage on this board. Record a marker above the
    # base 2 MiB boundary and prove its low-address alias remains independent.
    code += [_lui(8, 0x8021), _lui(10, 0x51a7), _ori(10, 10, 0xcafe),
             _sw(10, 8, 0x10), _lw(20, 8, 0x10), 0,
             _lui(8, 0x8001), _lw(21, 8, 0x10), 0]

    # Draw a red line with guest GP0 commands, then read two pixels back from
    # GPU VRAM. This validates the native GPU path from executed MIPS stores.
    code += [_lui(8, 0xbf80), _ori(8, 8, 0x1810),
             _lui(10, 0x0200), _ori(10, 10, 0x00ff), _sw(10, 8, 0),
             _sw(0, 8, 0), _lui(10, 1), _ori(10, 10, 0x10), _sw(10, 8, 0),
             _lw(11, 8, 4), 0, _lui(10, 0xc000), _sw(10, 8, 0),
             _sw(0, 8, 0), _lui(13, 0x0001), _ori(13, 13, 0x0002), _sw(13, 8, 0),
             _lw(19, 8, 0), 0]

    # Initial input sample, then a frame-polled loop. The trace must contain
    # both the pressed value and the released value after CSV frame events.
    code += [_lui(8, 0x1fba), _lhu(14, 8, 4), 0, _sw(14, 9, 0)]
    poll_pc = 0xbfc00000 + 4 * len(code)
    code += [_lhu(14, 8, 4), 0, _sw(14, 9, 0), _j(poll_pc), 0]
    return code


def _make_program_image(model: str, dma_overflow: bool = False) -> bytes:
    words = make_program(model)
    if dma_overflow:
        # MADR -4 aliases the final RAM word; the 16-byte transfer must fail
        # before the NAND reader consumes data or writes beyond RAM.
        words[words.index(_lui(11, 0x21))] = _addiu(11, 0, -4)
    if model in ("MEMM", "MEM(M)"):
        # The boot helper shares the NAND raw bank but is reached only through
        # the dynamic bank window.
        words += []
        subroutine = [_addiu(17, 0, 0x5a), 0x03e00008, 0, 0]
        while len(words) < 0x400 // 4:
            words.append(0)
        words.extend(subroutine)
    plain = bytearray(0x62000 if model == "MEMM" else 0x20000)
    struct.pack_into(f"<{len(words)}I", plain, 0, *words)
    if model in ("MEMM", "MEM(M)"):
        return encode_memm_boot(plain)
    return plain


def _nand_payload(profile: dict, model: str, dma_overflow: bool = False) -> dict[str, bytes]:
    payload: dict[str, bytes] = {}
    program = _make_program_image(model, dma_overflow)
    permutation = CHOCOVDR_PERM
    for region in profile["rom_regions"]:
        tag, size = region["tag"], region["size"]
        if tag == "maincpu:rom":
            payload[tag] = bytes(size)
            continue
        if tag == "nand":
            raw = bytearray([0xff]) * size
            raw[:len(program)] = program
            payload[tag] = bytes(raw)
            continue
        if tag.startswith("nand"):
            raw = bytearray([0xff]) * size
            if tag == "nand0":
                # Boot code resides in page 0x40 onward. DMA test data is on
                # page 0x300, deliberately beyond the copied boot range.
                encoded = encode_memn_bytes(program)
                for offset in range(0, len(encoded), 512):
                    boot_start = (0x40 + offset // 512) * 528
                    part = encoded[offset:offset + 512]
                    raw[boot_start:boot_start + len(part)] = part
                page = 0x300
                dma_bytes = bytearray(len(DATA_WORDS))
                for off in range(0, len(DATA_WORDS), 2):
                    plain_word = int.from_bytes(DATA_WORDS[off:off + 2], "little")
                    dma_bytes[off:off + 2] = _plain_to_passive_raw(plain_word, permutation).to_bytes(2, "big")
                start = page * 528
                raw[start:start + len(dma_bytes)] = dma_bytes
            payload[tag] = bytes(raw)
    return payload


def make_media(directory: Path, set_name: str, dma_overflow: bool = False) -> None:
    profile = PROFILES[set_name]
    model = "MEM(M)" if profile["board_type"] == "MEM(M)" else "MEM(N)"
    regions = _nand_payload(profile, model, dma_overflow)
    if model == "MEM(M)":
        regions["data"] = bytes(0x2000000)
    prepare.package_regions(set_name, profile, regions, directory,
                            {"synthetic_fixture": True, "rom_source": "generated MIPS"})


def run(harness: Path, directory: Path, *args: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([str(harness), "--system10", str(directory), *args],
                            capture_output=True, text=True, timeout=120)
    if result.stderr:
        print(result.stderr, end="")
    return result


def check_guest_result(result: subprocess.CompletedProcess[str], model: str,
                       trace_path: Path) -> None:
    result.check_returncode()
    required = ("System10 selftest: PASS", f"board={model}", "RAM=4194304 ROM=4194304")
    for text in required:
        if text not in result.stderr:
            raise RuntimeError(f"System 10 {model} run missing diagnostic {text!r}")
    if "fault=0" not in result.stderr:
        raise RuntimeError(f"System 10 {model} reported an adapter/bus fault")
    if not trace_path.is_file():
        raise RuntimeError("MIPS execution trace was not produced")
    lines = trace_path.read_text(encoding="utf-8").splitlines()
    if len(lines) < 10 or not lines[1].startswith("0,") or ",bfc00000," not in lines[1]:
        raise RuntimeError("guest did not execute from the PSX reset vector")
    if model == "MEM(N)" and "DMA=1 transfers/16 bytes" not in result.stderr:
        raise RuntimeError("MEM(N) channel 5 did not transfer its 16-byte NAND fixture")
    if model == "MEM(M)" and not any(",0000005a," in line for line in lines):
        raise RuntimeError("MEM(M) JALR helper did not execute through the banked fetch path")
    if model == "MEM(N)" and not any(
            row.split(",")[4 + 22] == f"{DMA_EXPECTED_WORD:08x}" for row in lines[1:]):
        raise RuntimeError("NAND DMA data did not reach upper PSX RAM")
    for register, expected, description in (
        ("r25", "00000005", "SIO1 ready status without console error flags"),
        ("r26", "000000ce", "SIO1 byte mode register lane"),
        ("r27", "00000050", "SIO1 halfword baud register lane"),
        ("r16", "00000008", "timer interrupt status"),
        ("r18", "00000000", "timer interrupt acknowledgement"),
        ("r19", "001f001f", "MIPS-issued GPU fill/readback"),
        ("r20", "51a7cafe", "upper-RAM sentinel"),
        ("r21", "00000000", "non-aliased low-RAM sentinel"),
    ):
        if not any(row.split(",")[4 + int(register[1:])] == expected for row in lines[1:]):
            raise RuntimeError(f"guest did not verify {description}")
    in1_pressed = any(",0000ffef," in line for line in lines)
    in1_released = any(",0000ffff," in line for line in lines)
    if not (in1_pressed and in1_released):
        raise RuntimeError("MIPS input polling did not observe the frame-indexed press and release")


def reject_before_guest(harness: Path, valid: Path, directory: Path,
                        *, mutate_config: str | None = None,
                        truncate_region: str | None = None) -> None:
    shutil.copytree(valid, directory)
    if mutate_config is not None:
        (directory / "board.ini").write_text(mutate_config, encoding="ascii")
    if truncate_region is not None:
        (directory / truncate_region).write_bytes(b"bad")
    trace = directory / "must-not-run.csv"
    result = run(harness, directory, "--frames", "1", "--trace", str(trace),
                 "--trace-blocks", "20")
    if result.returncode == 0:
        raise RuntimeError("invalid System 10 profile or region was accepted")
    if "System10: profile=" in result.stderr:
        raise RuntimeError("invalid System 10 input reached guest/device execution")
    if trace.is_file() and len(trace.read_text(encoding="utf-8").splitlines()) > 1:
        raise RuntimeError("invalid System 10 input reached guest CPU execution")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    args = parser.parse_args()
    harness = args.harness.resolve()
    if not harness.is_file():
        raise SystemExit(f"harness not found: {harness}; build with BOARD=1 first")
    with tempfile.TemporaryDirectory(prefix="pcsx-system10-test-") as temp:
        root = Path(temp)
        memm = root / "memm-mrdrilr2"
        memn = root / "memn-chocovdr"
        make_media(memm, "mrdrilr2")
        make_media(memn, "chocovdr")

        inputs = root / "inputs.csv"
        inputs.write_text("frame,control,pressed\n0,p1button1,1\n1,p1button1,0\n",
                          encoding="ascii")
        for set_name, directory, model in (("mrdrilr2", memm, "MEM(M)"),
                                            ("chocovdr", memn, "MEM(N)")):
            trace = root / f"{set_name}.csv"
            result = run(harness, directory, "--board-test", "--frames", "3",
                         "--inputs", str(inputs), "--trace", str(trace),
                         "--trace-blocks", "200000", "--output",
                         str(directory / "frame.ppm"))
            check_guest_result(result, model, trace)

        # The MEM(M) helper uses KSEG0 (9f400100), so this second execution
        # exercises the interpreter's dynamic bank instruction-cache refill.
        trace = root / "mrdrilr2-icache.csv"
        result = run(harness, memm, "--board-test", "--icache", "--frames", "3",
                     "--inputs", str(inputs), "--trace", str(trace),
                     "--trace-blocks", "200000")
        check_guest_result(result, "MEM(M)", trace)

        overflow = root / "memn-dma-overflow"
        make_media(overflow, "chocovdr", dma_overflow=True)
        result = run(harness, overflow, "--frames", "1")
        if result.returncode == 0 or "fault=1" not in result.stderr or \
                "DMA=0 transfers/0 bytes" not in result.stderr:
            raise RuntimeError("DMA exceeding RAM span did not fail before transferring")

        bad_config = memn / "board.ini"
        contents = bad_config.read_text(encoding="ascii")
        reject_before_guest(harness, memn, root / "unknown-set",
                            mutate_config=contents.replace("mame_set=chocovdr\n",
                                                          "mame_set=unknown-set\n"))
        reject_before_guest(harness, memn, root / "duplicate-key",
                            mutate_config=contents + "mame_set=chocovdr\n")
        reject_before_guest(harness, memn, root / "unsupported-board",
                            mutate_config=contents.replace("mame_set=chocovdr\n",
                                                          "mame_set=ptblank3\n"))
        reject_before_guest(harness, memn, root / "extra-region",
                            mutate_config=contents + "region.extra=extra.bin\nregion.extra.size=1\n")
        reject_before_guest(harness, memm, root / "truncated-flash",
                            truncate_region="rawflash.bin")
        reject_before_guest(harness, memn, root / "wrong-nand-size",
                            mutate_config=contents.replace("region.nand0.size=17301504",
                                                          "region.nand0.size=17301503"))
    print("System10 synthetic MEM(M)/MEM(N) guest, input, bank, NAND DMA and rejection checks PASS")


if __name__ == "__main__":
    main()
