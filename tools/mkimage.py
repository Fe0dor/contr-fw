#!/usr/bin/env python3
"""Образ обновления по Ethernet (FW-232, А11): контроль заголовка .bin и запись размера.

    python tools/mkimage.py build/target/contr-fw.bin build/target/contr-fw.upd
    python tools/mkimage.py --info build/target/contr-fw.upd

Заголовок struct fw_header (core/update.h) лежит по смещению 0x200: magic "CONTRFW1",
model[8], revision, версия a.b.c, image_size. Файл .upd — тот же образ с заполненным
image_size; CRC32 всего файла передаётся в SYST:UPD:BEGIN и считается интерфейсом.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

HEADER_OFFSET = 0x200
HEADER_FMT = "<8s8sBBBBI3I"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
MAGIC = b"CONTRFW1"


def parse_header(data: bytes) -> dict:
    if len(data) < HEADER_OFFSET + HEADER_SIZE:
        raise ValueError("образ короче заголовка")
    magic, model, rev, a, b, c, size, *_ = struct.unpack_from(HEADER_FMT, data, HEADER_OFFSET)
    if magic != MAGIC:
        raise ValueError(f"нет заголовка CONTRFW1 по смещению {HEADER_OFFSET:#x}")
    return {
        "model": model.rstrip(b"\0").decode("ascii"),
        "revision": rev,
        "version": f"{a}.{b}.{c}",
        "image_size": size,
    }


def make(src: Path, dst: Path) -> dict:
    data = bytearray(src.read_bytes())
    # выровнять до 4 байт: прошивка пишет словами
    while len(data) % 4:
        data.append(0xFF)
    info = parse_header(bytes(data))
    struct.pack_into("<I", data, HEADER_OFFSET + 20, len(data))
    dst.write_bytes(bytes(data))
    info["image_size"] = len(data)
    info["crc32"] = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    return info


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("src", type=Path)
    parser.add_argument("dst", type=Path, nargs="?")
    parser.add_argument("--info", action="store_true", help="показать заголовок и выйти")
    args = parser.parse_args(argv)
    if args.info or args.dst is None:
        data = args.src.read_bytes()
        info = parse_header(data)
        info["crc32"] = zlib.crc32(data) & 0xFFFFFFFF
        info["file_size"] = len(data)
    else:
        info = make(args.src, args.dst)
    for k, v in info.items():
        print(f"{k}: {v:#010x}" if k == "crc32" else f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
