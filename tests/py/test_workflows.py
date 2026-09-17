import struct

import pytest

from contr_emu.device import Device
from contr_ui.workflows import UpdateImage, parse_script, payload


def image():
    data = bytearray(b"\xff" * 1024)
    struct.pack_into("<8s8sBBBBI3I", data, 0x200, b"CONTRFW1", b"CONTR", 1, 0, 3, 0,
                     len(data), 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
    return bytes(data)


def test_ui_image_commands_confirm_and_rollback():
    for confirm in (True, False):
        dev = Device()
        dev.connect()
        update = UpdateImage.read(image())
        for cmd in update.commands():
            assert payload(dev.execute("tcp", cmd)) == "OK"
        dev.reboot()
        dev.connect()
        assert payload(dev.execute("tcp", "SYST:UPD:STAT?")).startswith("TRIAL,")
        if confirm:
            assert payload(dev.execute("tcp", "SYST:UPD:CONFIRM")) == "OK"
        dev.advance(60001)
        assert dev.active_bank == (2 if confirm else 1)


def test_second_unconfirmed_boot_rolls_back():
    dev = Device()
    for cmd in UpdateImage.read(image()).commands():
        assert dev.execute("console", cmd) == "OK"
    dev.reboot()
    assert dev.active_bank == 2
    dev.reboot()
    assert dev.active_bank == 1
    assert "rolled back" in dev.execute("console", "SYST:ERR?")


def test_image_rejects_unpatched_size():
    data = bytearray(image())
    data[0x214:0x218] = b"\xff" * 4
    with pytest.raises(ValueError, match="Размер"):
        UpdateImage.read(bytes(data))


def test_script_directives():
    assert parse_script("# comment\nSAFE\n#wait 50\n#prompt Press USER\n*IDN?\n") == [
        ("command", "SAFE"), ("wait", 50), ("prompt", "Press USER"), ("command", "*IDN?")]
    with pytest.raises(ValueError):
        parse_script("#wait -1")


def test_payload_preserves_semicolon_records():
    assert payload("G7;A;B") == "A;B"
    assert payload("ERR:BUSY") == "ERR:BUSY"
