"""Генератор единого источника: FW-239…FW-244, схема SDK и правила платформы (PLG-026)."""

from __future__ import annotations

import copy
import json
import re
from pathlib import Path

import jsonschema
import pytest
import yaml

import gen_board

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "board" / "rev1" / "relays.yaml"
GEN = ROOT / "gen"
SCHEMA = json.loads((Path(__file__).parent / "schemas" / "interlock-table.schema.json").read_text("utf-8"))


@pytest.fixture(scope="module")
def table() -> gen_board.Table:
    return gen_board.load_all()


@pytest.fixture(scope="module")
def plugin_file(table: gen_board.Table) -> dict:
    return gen_board.gen_interlock_json(table)


def test_all_81_relays_present(table: gen_board.Table) -> None:
    names = [r.name for r in table.relays]
    assert len(names) == 81  # FW-119
    expected = (
        [f"V1.{i}" for i in range(1, 9)] + [f"V2.{i}" for i in range(1, 8)]
        + [f"V3.{i}" for i in range(1, 11)] + [f"V4.{i}" for i in range(1, 5)]
        + [f"M{i}" for i in range(1, 20)] + [f"A{i}" for i in range(1, 6)]
        + [f"CH1.{i}" for i in range(1, 6)] + ["CH2.1"]
        + [f"D{d}_G{g}" for d in range(1, 7) for g in (1, 2)]
        + ["PM1", "PM2", "PM3", "RAtoDUT", "RBtoDUT"] + [f"L{i}" for i in range(1, 6)]
    )
    assert names == expected  # канонический порядок FW-120


def test_plugin_file_matches_sdk_schema(plugin_file: dict) -> None:
    jsonschema.Draft202012Validator(SCHEMA).validate(plugin_file)  # FW-241
    assert set(plugin_file) == {"table_version", "checksum", "relays", "groups"}
    assert "power_relays" not in plugin_file


def test_plugin_file_cross_rules(plugin_file: dict) -> None:
    """Правила модели платформы, невыразимые схемой (PLG-026)."""
    relays = set(plugin_file["relays"])
    names = [g["name"] for g in plugin_file["groups"]]
    assert len(names) == len(set(names))
    for g in plugin_file["groups"]:
        assert set(g["relays"]) <= relays
        assert len(g["relays"]) == 2


def test_relays_without_loads(plugin_file: dict, table: gen_board.Table) -> None:
    assert len(plugin_file["relays"]) == 76  # FW-242
    loads = {r.number for r in table.relays if r.name.startswith("L")}
    assert loads == {77, 78, 79, 80, 81}
    assert not loads & set(plugin_file["relays"])


def test_pairwise_expansion(plugin_file: dict) -> None:
    by_group: dict[str, list[dict]] = {}
    for g in plugin_file["groups"]:
        by_group.setdefault(g["name"].split("__")[0], []).append(g)
    assert len(by_group["M"]) == 171  # FW-243
    assert len(by_group["V"]) == 406
    assert len(by_group["A"]) == 10
    assert len(by_group["CH1"]) == 10
    for d in range(1, 7):
        assert len(by_group[f"D{d}"]) == 1
    assert len(by_group["RA"]) == 3 and len(by_group["RB"]) == 3
    # порядок пар: по номеру первого, затем второго реле
    pairs = [tuple(g["relays"]) for g in by_group["M"]]
    assert pairs == sorted(pairs)
    assert all(a < b for a, b in pairs)
    # имя пары по FW-126
    assert by_group["M"][0]["name"] == "M__M1__M2"
    assert {"name": "M__M4__M5", "relays": [33, 34]} in by_group["M"]
    assert by_group["V"][0]["name"] == "V__V1_1__V1_2"
    assert all(re.match(r"^[A-Za-z][A-Za-z0-9_]*$", g["name"]) for g in plugin_file["groups"])


def test_checksum_format_and_stability(table: gen_board.Table) -> None:
    assert re.match(r"^sha256:[0-9a-f]{64}$", table.checksum())  # FW-131
    assert gen_board.load_table(SOURCE).checksum() == table.checksum()


def test_generated_files_are_current(table: gen_board.Table) -> None:
    assert gen_board.check_all(table, GEN) == []  # FW-240


def _write_variant(tmp_path: Path, mutate) -> Path:
    raw = yaml.safe_load(SOURCE.read_text("utf-8"))
    mutate(raw)
    path = tmp_path / "relays.yaml"
    path.write_text(yaml.safe_dump(raw, allow_unicode=True, sort_keys=False), "utf-8")
    return path


def test_group_change_changes_checksum_and_outputs(tmp_path: Path, table: gen_board.Table) -> None:
    """FW-239: правка группы одного реле меняет все артефакты и сумму."""

    def mutate(raw: dict) -> None:
        entry = next(e for e in raw["relays"] if e["name"] == "PM1")
        entry["groups"] = ["CH1"]

    changed = gen_board.load_table(_write_variant(tmp_path, mutate))
    assert changed.checksum() != table.checksum()
    before, after = gen_board.render_all(table), gen_board.render_all(changed)
    relay_outputs = [k for k in before if not k.startswith("signal_table")]
    assert all(before[k] != after[k] for k in relay_outputs)


def test_comment_does_not_change_checksum(tmp_path: Path, table: gen_board.Table) -> None:
    def mutate(raw: dict) -> None:
        raw["relays"][0]["note"] = "новое примечание"

    assert gen_board.load_table(_write_variant(tmp_path, mutate)).checksum() == table.checksum()


def test_duplicate_number_rejected(tmp_path: Path) -> None:
    def mutate(raw: dict) -> None:
        raw["relays"][1]["number"] = raw["relays"][0]["number"]

    with pytest.raises(gen_board.SourceError, match="FW-244"):
        gen_board.load_table(_write_variant(tmp_path, mutate))


def test_retired_number_rejected(tmp_path: Path) -> None:
    def mutate(raw: dict) -> None:
        raw["retired"] = [raw["relays"][5]["number"]]

    with pytest.raises(gen_board.SourceError, match="FW-244"):
        gen_board.load_table(_write_variant(tmp_path, mutate))


def test_new_relay_must_take_next_number(tmp_path: Path) -> None:
    """Удалить реле и добавить новое: его номер больше всех прежних (FW-244)."""

    def bad(raw: dict) -> None:
        removed = raw["relays"].pop(10)
        raw["relays"].append({"name": "NEW1", "number": removed["number"], "block": "int",
                              "address": "PG7", "groups": []})
        raw["retired"] = [removed["number"]]

    with pytest.raises(gen_board.SourceError, match="FW-244"):
        gen_board.load_table(_write_variant(tmp_path, bad))

    def good(raw: dict) -> None:
        removed = raw["relays"].pop(10)
        raw["retired"] = [removed["number"]]
        raw["relays"].append({"name": "NEW1", "number": raw["next_number"], "block": "int",
                              "address": "PG7", "groups": []})
        raw["next_number"] += 1

    t = gen_board.load_table(_write_variant(tmp_path, good))
    assert max(r.number for r in t.relays) == 82


def test_missing_groups_key_is_rejected_by_schema(plugin_file: dict) -> None:
    broken = copy.deepcopy(plugin_file)
    del broken["groups"]
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.Draft202012Validator(SCHEMA).validate(broken)


def test_signals_present_and_disjoint(table: gen_board.Table) -> None:
    names = {sg.name for sg in table.signals}
    assert {"PWRON_A", "PWRON_B", "PWRON_RSP", "CS_PWR", "HLG", "USER", "SR_TEST_IN", "LD1"} <= names
    assert {f"C{i}" for i in range(1, 21)} <= names
    assert {f"PWROK{i}" for i in range(1, 6)} <= names
    assert not names & {r.name for r in table.relays}
    assert not {sg.address for sg in table.signals} & {r.address for r in table.relays}


def test_address_uniqueness_and_format(table: gen_board.Table) -> None:
    addresses = [r.address for r in table.relays]
    assert len(addresses) == len(set(addresses))
    assert all(gen_board.PIN_RE.match(a) or gen_board.SR_RE.match(a) for a in addresses)


@pytest.mark.parametrize('address', ['PA16', 'PK99', 'PA01', 'SR0.3.A', 'SR1.4.H'])
def test_physical_address_bounds(tmp_path, address):
    def mutate(raw):
        raw['relays'][0]['address'] = address
    with pytest.raises(gen_board.SourceError):
        gen_board.load_table(_write_variant(tmp_path, mutate))


def test_storage_field_bounds(tmp_path):
    def number(raw):
        raw['next_number'] = 257
        raw['relays'][0]['number'] = 256
    def groups(raw):
        raw['groups'] += [{'name': f'Extra{i}'} for i in range(17)]
    for mutate in (number, groups):
        with pytest.raises(gen_board.SourceError):
            gen_board.load_table(_write_variant(tmp_path, mutate))


def test_signal_gpio_bounds(tmp_path):
    raw = yaml.safe_load(gen_board.DEFAULT_SIGNALS.read_text('utf-8'))
    raw['signals'][0]['address'] = 'PA16'
    path = tmp_path / 'signals.yaml'
    path.write_text(yaml.safe_dump(raw), 'utf-8')
    with pytest.raises(gen_board.SourceError):
        gen_board.load_signals(path, ())
