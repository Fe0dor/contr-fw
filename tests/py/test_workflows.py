import struct

import pytest

from contr_emu.device import Device
from contr_ui.workflows import UpdateImage, parse_script, payload


def image():
    data = bytearray(b"\xff" * 1024)
    struct.pack_into("<II", data, 0, 0x20080000, 0x08000225)
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


@pytest.mark.parametrize('sp,reset', [
    (0x20000000, 0x08000225), (0x20080008, 0x08000225),
    (0x20080001, 0x08000225), (0x20080000, 0x08000224),
    (0x20080000, 0x08000001), (0x20080000, 0x08000401),
])
def test_invalid_vectors_rejected_with_valid_crc(sp, reset):
    import base64
    import zlib
    data = bytearray(image())
    struct.pack_into('<II', data, 0, sp, reset)
    with pytest.raises(ValueError, match='SP/Reset_Handler'):
        UpdateImage.read(bytes(data))
    dev = Device()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    assert dev.execute('console', f'SYST:UPD:BEGIN {len(data)},{crc:08x},0.3.0') == 'OK'
    for off in range(0, len(data), 256):
        block = base64.b64encode(data[off:off+256]).decode()
        assert dev.execute('console', f'SYST:UPD:DATA {block}') == 'OK'
    assert dev.execute('console', 'SYST:UPD:COMMIT') == 'ERR:UPD_HEADER,vectors'
    assert dev.boot_bank == 1
