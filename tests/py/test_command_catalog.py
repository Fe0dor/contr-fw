"""Keep the executable UI reference in step with the implemented C commands."""
import re
from pathlib import Path

from contr_ui.command_catalog import COMMAND_CATALOG
from contr_ui.workflows import COMMANDS


def test_catalog_matches_firmware_table_and_argument_requirements():
    source = (Path(__file__).resolve().parents[2] / 'core/cmd.c').read_text('utf-8')
    table = source.split('static const struct cmd_desc table[] = {', 1)[1].split('};', 1)[0]
    descriptors = re.findall(r'\{"([^"\n]+)", (\d+), (\d+), ([^,]+),', table)
    catalog = {item['name']: item for item in COMMAND_CATALOG}
    assert len(catalog) == len(COMMAND_CATALOG) == len(descriptors)
    assert set(catalog) == {name for name, *_ in descriptors}
    for name, minimum, maximum, flags in descriptors:
        item = catalog[name]
        assert bool(item['args']) == (int(maximum) > 0), name
        assert item['console'] == ('CMDF_CONSOLE_ONLY' in flags), name
        assert item['bringup'] == ('CMDF_BRINGUP' in flags), name
        assert item['description']
    assert {item['name'] for item in COMMAND_CATALOG if not item['bringup']} == set(COMMANDS)
