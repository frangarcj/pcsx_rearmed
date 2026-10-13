#!/usr/bin/env python3
"""Exercise the native System 12 adapter without any game ROMs or BIOS."""
import argparse
import wave
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pcsx-system12-test-") as tmp:
        regions = Path(tmp)
        # Distinct bank edges expose accidental 2 MiB/512 KiB aliasing. The
        # reset program is only 'j bfc00000; nop', generated locally.
        main_rom = bytearray(0x400000)
        struct.pack_into("<II", main_rom, 0, 0x0bf00000, 0)
        struct.pack_into("<I", main_rom, 0x3ffffc, 0x1234abcd)
        (regions / "main.bin").write_bytes(main_rom)
        banked = bytearray(0x2000000)
        for bank in range(16):
            struct.pack_into("<I", banked, bank * 0x200000, 0xdead0000 | bank)
            struct.pack_into("<I", banked, (bank + 1) * 0x200000 - 4, 0xbeef0000 | bank)
        (regions / "banked.bin").write_bytes(banked)
        sub_rom = bytearray(0x80000)
        struct.pack_into(">I", sub_rom, 0, 0x100)
        struct.pack_into(">H", sub_rom, 0x100, 0x40fe)  # H8 BRA self
        (regions / "sub.bin").write_bytes(sub_rom)
        (regions / "wave.bin").write_bytes(bytes(0x1000000))
        command = [str(args.harness.resolve()), "--system12", str(regions),
                   "--board-test", "--frames", "1", "--output", str(regions / "frame.ppm")]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        print(result.stderr, end="")
        result.check_returncode()
        if "System12 selftest: PASS" not in result.stderr:
            raise RuntimeError("board selftest did not run")
        if "System12 EEPROM timing: PASS" not in result.stderr:
            raise RuntimeError("AT28C16 bus timing selftest did not run")
        if "pc=bfc00000" not in result.stderr:
            raise RuntimeError("synthetic reset program did not execute correctly")
        if not (regions / "frame.ppm").read_bytes().startswith(b"P6\n"):
            raise RuntimeError("frame output missing")
        # MAME's COH-716/altbank geometry: 16 MiB RAM, the reset copy, and a
        # deliberately short 8 MiB bank region exercise dynamic bounds.
        variant = regions / "coh716-altbank"
        variant.mkdir()
        (variant / "main.bin").write_bytes(main_rom)
        (variant / "banked.bin").write_bytes(banked[:0x800000])
        (variant / "sub.bin").write_bytes(sub_rom)
        (variant / "wave.bin").write_bytes(bytes(0x800000))
        (variant / "board.cfg").write_text(
            "schema=1\nmame_set=sws2001\nmachine=coh716\nstate=namcos12_altbank_state\n"
            "ram_size=16777216\nmain_size=4194304\nbank_size=8388608\n"
            "sub_size=524288\nwave_size=8388608\nboot_hack=1\nalt_bank=1\n"
            "tektagt_dma=0\nunsupported=none\n")
        main_rom[0x20280:0x2028c] = b"BOOT-HACK-12"
        (variant / "main.bin").write_bytes(main_rom)
        profile_result = subprocess.run([str(args.harness.resolve()), "--system12", str(variant),
            "--board-test", "--frames", "1", "--output", str(variant / "frame.ppm")],
            capture_output=True, text=True, timeout=30)
        print(profile_result.stderr, end="")
        profile_result.check_returncode()
        if "System12 selftest: PASS" not in profile_result.stderr or "RAM=16777216" not in profile_result.stderr:
            raise RuntimeError("COH-716/altbank profile failed dynamic RAM or bank/DMA validation")
        if "System12 EEPROM timing: PASS" not in profile_result.stderr:
            raise RuntimeError("COH-716 AT28C16 bus timing selftest did not run")
        # Gun buses use real 16-bit lanes, including the mirrored window.
        for name, machine, state, alt in [("ptblank2", "coh700b", "ptblank2_state", 0),
                                          ("golgo13", "golgo13", "golgo13_state", 1)]:
            gun = regions / name
            gun.mkdir()
            for filename in ("main.bin", "banked.bin", "sub.bin", "wave.bin"):
                (gun / filename).write_bytes((variant / filename).read_bytes())
            (gun / "board.cfg").write_text(
                f"schema=1\nmame_set={name}\nmachine={machine}\nstate={state}\n"
                "ram_size=4194304\nmain_size=4194304\nbank_size=8388608\n"
                f"sub_size=524288\nwave_size=8388608\nboot_hack=1\nalt_bank={alt}\n"
                "tektagt_dma=0\nunsupported=none\n")
            run = subprocess.run([str(args.harness.resolve()), "--system12", str(gun),
                "--board-test", "--frames", "1", "--output", str(gun / "frame.ppm")],
                capture_output=True, text=True, timeout=30)
            if run.returncode or "System12 selftest: PASS" not in run.stderr:
                raise RuntimeError(f"{name} gun bus / ADC selftest failed: {run.stderr}")
        # Declared unavailable daughterboards are refused before CPU execution.
        unsupported = regions / "unsupported"
        unsupported.mkdir()
        for name in ("main.bin", "banked.bin", "sub.bin", "wave.bin"):
            (unsupported / name).write_bytes((regions / name).read_bytes())
        (unsupported / "board.cfg").write_text(
            "schema=1\nmame_set=aplarail\nmachine=aplarail\nstate=namcos12_state\n"
            "ram_size=4194304\nmain_size=4194304\nbank_size=33554432\n"
            "sub_size=524288\nwave_size=16777216\nboot_hack=0\nalt_bank=0\n"
            "tektagt_dma=0\nunsupported=future_fixture_device\n")
        rejected = subprocess.run([str(args.harness.resolve()), "--system12", str(unsupported),
            "--frames", "1"], capture_output=True, text=True, timeout=30)
        if rejected.returncode != 1 or "unimplemented board device" not in rejected.stderr:
            raise RuntimeError("profile declaring an unknown fixture device was not rejected")
        inputs = regions / "inputs.csv"
        inputs.write_text("frame,control,pressed\n0,coin1,1\n1,coin1,0\n2,p1start,1\n3,coin2,1\n")
        audio = regions / "fixture.wav"
        result = subprocess.run([str(args.harness.resolve()), "--system12", str(regions),
            "--board-test", "--audio-test-fixture", "--frames", "4", "--inputs", str(inputs),
            "--audio", str(audio), "--output", str(regions / "io.ppm")],
            capture_output=True, text=True, timeout=30)
        print(result.stderr, end="")
        result.check_returncode()
        if "System12: JAMMA=ff/7f/ef/ff" not in result.stderr:
            raise RuntimeError("frame-indexed JAMMA events did not reach the mapped active-low bytes")
        with wave.open(str(audio), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 88200):
                raise RuntimeError("WAV format is not stereo PCM16 at 88.2 kHz")
            samples = wav.readframes(wav.getnframes())
            if wav.getnframes() == 0 or not any(samples):
                raise RuntimeError("fixture WAV has no generated nonzero C352 samples")
        inputs.write_text(",0,coin1,1\n")
        result = subprocess.run(command + ["--inputs", str(inputs)], capture_output=True, text=True, timeout=30)
        if result.returncode != 2 or "invalid inputs CSV" not in result.stderr:
            raise RuntimeError("empty CSV fields were silently accepted")
        # A short region must fail before any guest CPU execution.
        (regions / "main.bin").write_bytes(b"bad")
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if result.returncode != 1 or "must contain exactly" not in result.stderr:
            raise RuntimeError("truncated region was not rejected cleanly")
    print("System12 synthetic board and malformed-region checks PASS")


if __name__ == "__main__":
    main()
