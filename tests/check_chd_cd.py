#!/usr/bin/env python3
"""Build private real CHD fixtures with MAME chdman and exercise the backend."""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--chdman", default="chdman")
    args = parser.parse_args()
    chdman = shutil.which(args.chdman)
    if not chdman:
        parser.error("MAME chdman is required to create real CD CHD fixtures")
    with tempfile.TemporaryDirectory(prefix="pcsx-chd-cd-") as temp:
        root = pathlib.Path(temp)
        data = root / "data.bin"
        audio = root / "audio.bin"
        raw = root / "raw.bin"
        # Cooked MODE1 payloads carry a sector-specific signature.
        with data.open("wb") as f:
            for lba in range(5):
                f.write(bytes(((lba * 17 + i * 3 + 9) & 255) for i in range(2048)))
        # Big-endian Red Book samples, as consumed by MAME's CDDA device.
        with audio.open("wb") as f:
            for sector in range(6):
                for sample in range(588):
                    f.write(struct.pack(">hh", 0x1234 + sector, -2 - sector))
        with raw.open("wb") as f:
            for sector in range(3):
                f.write(bytes([0xA0 + sector]) * 16)
                f.write(bytes(((sector * 23 + i * 5 + 7) & 255) for i in range(2048)))
                f.write(bytes([0xE0 + sector]) * (2352 - 16 - 2048))
        cue = root / "disc.cue"
        cue.write_text(
            'FILE "data.bin" BINARY\n'
            '  TRACK 01 MODE1/2048\n'
            '    INDEX 01 00:00:00\n'
            '    POSTGAP 00:00:02\n'
            'FILE "audio.bin" BINARY\n'
            '  TRACK 02 AUDIO\n'
            '    INDEX 00 00:00:00\n'
            '    INDEX 01 00:00:02\n'
            'FILE "raw.bin" BINARY\n'
            '  TRACK 03 MODE1/2352\n'
            '    INDEX 01 00:00:00\n',
            encoding="ascii",
        )
        chd = root / "disc.chd"
        run(chdman, "createcd", "-i", str(cue), "-o", str(chd))
        info = subprocess.run([chdman, "info", "-i", str(chd), "-v"],
                              check=True, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT).stdout
        if "TRACK:2" not in info or "PREGAP:2" not in info or "POSTGAP:2" not in info or "TRACK:3" not in info:
            raise RuntimeError(f"fixture does not exercise pregap/postgap metadata:\n{info}")
        bad_type = root / "bad-type.chd"
        shutil.copyfile(chd, bad_type)
        raw = bad_type.read_bytes()
        if raw.count(b" TYPE:MODE1 ") != 1:
            raise RuntimeError("expected one synthetic cooked MODE1 metadata type")
        bad_type.write_bytes(raw.replace(b" TYPE:MODE1 ", b" TYPE:MODE2 ", 1))
        bad_geometry = root / "bad-geometry.chd"
        shutil.copyfile(chd, bad_geometry)
        raw = bad_geometry.read_bytes()
        if raw.count(b"FRAMES:5") != 1:
            raise RuntimeError("expected one synthetic first-track frame count")
        bad_geometry.write_bytes(raw.replace(b"FRAMES:5", b"FRAMES:0", 1))
        bad_decimal = root / "bad-decimal.chd"
        run(chdman, "copy", "-i", str(chd), "-o", str(bad_decimal), "-c", "none")
        run(chdman, "addmeta", "-i", str(bad_decimal), "-t", "CHT2", "-ix", "0", "-vt",
            "TRACK:1 TYPE:MODE1 SUBTYPE:NONE FRAMES:18446744073709551617 "
            "PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:2")
        mode2 = root / "mode2.bin"
        with mode2.open("wb") as stream:
            for lba in range(2):
                sector = bytearray(2352)
                sector[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
                sector[15] = 2
                sector[18] = sector[22] = 0x20 if lba else 0x08
                for i in range(2048): sector[24 + i] = (i * 7 + lba * 13) & 255
                stream.write(sector)
        mode2_cue = root / "mode2.cue"
        mode2_cue.write_text('FILE "mode2.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
        mode2_chd = root / "mode2.chd"
        run(chdman, "createcd", "-i", str(mode2_cue), "-o", str(mode2_chd), "-c", "none")
        subprocess.run([args.binary, str(chd), str(bad_type), str(bad_geometry),
                        str(bad_decimal), str(mode2_chd)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
