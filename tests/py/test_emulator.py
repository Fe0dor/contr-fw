"""Эмулятор: контракт FW-249, совпадение таблицы с файлом данных (FW-250), протокол шага 2."""

from __future__ import annotations

import json
import zlib
from pathlib import Path

import pytest

from contr_emu import Emulator
from contr_emu.device import Device

ROOT = Path(__file__).resolve().parents[2]


def test_emulator_contract_nonblocking_read() -> None:
    emu = Emulator()  # без аргументов (FW-249)
    assert emu.supports_nonblocking_poll is True
    with pytest.raises(TimeoutError):
        emu.read(timeout_s=0)
    emu.write(b"*IDN?\n")
    reply = emu.read(timeout_s=0)
    assert reply.startswith(b"G1;TESTDUT,CONTR-R1,UNPROVISIONED,0.4.0\n")
    with pytest.raises(TimeoutError):
        emu.read(0)


def test_interlock_table_matches_plugin_file() -> None:
    """FW-250: версия и сумма из INTERLOCK:LIST? равны полям файла данных."""
    plugin = json.loads((ROOT / "gen" / "interlock-table.json").read_text("utf-8"))
    emu = Emulator()
    emu.write(b"INTERLOCK:LIST?\n")
    last = emu.read().decode().strip().split(";")[-1]
    version, checksum = last.split(",")
    assert version == plugin["table_version"]
    assert checksum == plugin["checksum"]
    assert emu.read.__self__.device.execute("console", "INTERLOCK:LIST?").count(";") == 81


def test_console_arbitration_and_generation() -> None:
    dev = Device()
    assert dev.connect()
    assert not dev.connect()  # второй клиент
    gen_before = dev.generation
    assert dev.execute("console", "SYST:UPD:ABORT") == "ERR:BUSY"
    assert dev.execute("console", "SYST:PROV:SERIAL X") == "ERR:CONSOLE_ONLY"
    assert dev.execute("console", "SAFE") == "OK"
    assert dev.execute("tcp", "*IDN?").startswith(f"G{gen_before + 1};")
    dev.disconnect()
    assert dev.execute("console", "SYST:PROV:SERIAL CONTR-0001") == "OK"
    assert dev.execute("console", "SYST:PROV:SERIAL CONTR-0002") == "ERR:PROVISIONED"
    assert dev.execute("console", "SYST:SAFE?").startswith("3,CLIENT_LOST,")


def test_update_cycle() -> None:
    dev = Device()
    img = bytearray(b"\xff" * 1024)
    hdr = b"CONTRFW1" + b"CONTR\0\0\0" + bytes([1, 0, 3, 0]) + (1024).to_bytes(4, "little") + b"\xff" * 12
    img[:8] = (0x20080000).to_bytes(4, "little") + (0x08000225).to_bytes(4, "little")
    img[0x200:0x200 + len(hdr)] = hdr
    crc = zlib.crc32(bytes(img)) & 0xFFFFFFFF
    assert dev.execute("console", f"SYST:UPD:BEGIN 1024,{crc:08x},0.4.0") == "OK"
    import base64
    for off in range(0, 1024, 256):
        block = base64.b64encode(bytes(img[off:off + 256])).decode()
        assert dev.execute("console", f"SYST:UPD:DATA {block}") == "OK"
    assert dev.execute("console", "SYST:UPD:COMMIT") == "OK"
    dev.reboot()
    assert dev.execute("console", "SYST:UPD:STAT?") == "TRIAL,0,0"
    assert dev.execute("console", "SYST:UPD:CONFIRM") == "OK"
    assert dev.execute("console", "SYST:UPD:CONFIRM") == "ERR:UPD_SEQ"
    # без подтверждения — откат по таймеру
    dev2 = Device()
    dev2.trial_bank = 2
    dev2.boot_bank = 2
    dev2.reboot()
    assert dev2.upd_state == "TRIAL"
    dev2.advance(60001)
    assert dev2.boot_bank == 1


def test_conf_and_errors() -> None:
    dev = Device()
    conf = dev.execute("console", "SYST:CONF?")
    assert conf.count("=") == 50
    assert "NET_DEFAULT_ADDR=192.168.0.20;" in conf
    assert dev.execute("console", "FOO") == "ERR:UNKNOWN_CMD"
    assert dev.execute("console", "SYST:ERR?") == "UNKNOWN_CMD"
    assert dev.execute("console", "SYST:ERR?") == "NONE"
