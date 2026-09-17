"""Protocol helpers shared by the GUI and headless acceptance tests."""
from __future__ import annotations

import base64
import re
import struct
import zlib
from dataclasses import dataclass

COMMANDS = (
    "*IDN?", "SAFE", "SYST:SAFE?", "SYST:ERR?", "SYST:LOG?", "SYST:CONF?",
    "SYST:NET?", "SYST:PROV:SERIAL", "SYST:PROV:NET", "SYST:UPD:BEGIN",
    "SYST:UPD:DATA", "SYST:UPD:COMMIT", "SYST:UPD:ABORT", "SYST:UPD:STAT?",
    "SYST:UPD:CONFIRM", "INTERLOCK:LIST?", "TEST:ALL?",
)


def payload(reply: str) -> str:
    return re.sub(r"^G\d+;", "", reply)


def parse_script(text: str) -> list[tuple[str, str | int]]:
    steps = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith("#wait "):
            try:
                ms = int(line[6:])
            except ValueError as exc:
                raise ValueError(f"Строка {n}: неверная пауза") from exc
            if ms < 0:
                raise ValueError(f"Строка {n}: отрицательная пауза")
            steps.append(("wait", ms))
        elif line.startswith("#prompt "):
            steps.append(("prompt", line[8:]))
        elif line and not line.startswith("#"):
            if not line.isascii():
                raise ValueError(f"Строка {n}: команда должна быть ASCII")
            steps.append(("command", line))
    return steps


@dataclass(frozen=True)
class UpdateImage:
    data: bytes
    model: str
    revision: int
    version: str
    crc: int

    @classmethod
    def read(cls, data: bytes) -> "UpdateImage":
        if not 0x224 <= len(data) <= 896 * 1024:
            raise ValueError("Размер образа вне допустимого диапазона")
        magic, model, rev, a, b, c, size, *_ = struct.unpack_from("<8s8sBBBBI3I", data, 0x200)
        if magic != b"CONTRFW1" or model.rstrip(b"\0") != b"CONTR" or rev != 1:
            raise ValueError("Неверный заголовок, модель или ревизия образа")
        if size != len(data):
            raise ValueError("Размер в заголовке не совпадает с файлом; выберите .upd")
        if (a, b, c) < (0, 2, 0):
            raise ValueError("Версия ниже минимальной 0.2.0")
        return cls(data, "CONTR", rev, f"{a}.{b}.{c}", zlib.crc32(data) & 0xFFFFFFFF)

    def commands(self, block_size: int = 256) -> list[str]:
        if not 1 <= block_size <= 256:
            raise ValueError("Недопустимый размер блока")
        result = [f"SYST:UPD:BEGIN {len(self.data)},{self.crc:08x},{self.version}"]
        result += ["SYST:UPD:DATA " + base64.b64encode(self.data[i:i + block_size]).decode("ascii")
                   for i in range(0, len(self.data), block_size)]
        return result + ["SYST:UPD:COMMIT"]
