from pathlib import Path
from flash import build_script


def test_dual_loader_is_selected_before_connect_and_image_only():
    script = build_script(Path('/tmp/contr.hex'), Path('/tmp/dual'))
    assert script.index('JLinkDevicesXMLPath') < script.index('connect')
    assert script.index('R0') < script.index('connect') < script.index('R1')
    assert 'loadfile /tmp/contr.hex' in script
    assert 'erase' not in script
    assert '40023C14' not in script  # Ordinary flashing never changes options.


def test_reset_only_does_not_load_or_erase_flash():
    script = build_script(None)
    assert 'loadfile' not in script and 'erase' not in script
    assert script.endswith('r\ng\nq\n')
