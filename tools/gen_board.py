#!/usr/bin/env python3
"""Генератор единого источника таблицы реле (ТЗ 19, раздел 3.12, архитектура А12).

Читает board/<rev>/relays.yaml и порождает:

* gen/relay_table.h, gen/relay_table.c — таблица реле, адреса и маски групп для прошивки;
* gen/interlock-table.json — файл данных плагина по схеме SDK interlock-table.schema.json
  (FW-241): реле без L* (FW-242), группы развёрнуты попарно (FW-243);
* gen/relay-numbers.json — имя → номер, блок, адрес, группы: для эмулятора и интерфейса;
* gen/interlock-groups.md — таблица шинных групп для документации (FW-239).

Контрольная сумма (FW-131, А12): sha256 канонического JSON самой таблицы —
{"relays": [...], "table_version": ...} с сортированными ключами и без пробелов.
Комментарии и примечания YAML в сумму не входят.

Режимы:
    python tools/gen_board.py            # перегенерировать gen/
    python tools/gen_board.py --check    # сверить gen/ с источником, код 1 при расхождении (FW-240)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

def _utf8_console() -> None:
    """Терминал Windows по умолчанию в cp1251/cp866: русские сообщения не должны ронять скрипт."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_utf8_console()

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = ROOT / "board" / "rev1" / "relays.yaml"
DEFAULT_SIGNALS = ROOT / "board" / "rev1" / "signals.yaml"
DEFAULT_OUT = ROOT / "gen"
SIGNAL_DIRS = ("out", "in", "dut")
PULLS = ("none", "up", "down")

NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")
GROUP_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
PIN_RE = re.compile(r"^P([A-K])(\d{1,2})$")
SR_RE = re.compile(r"^SR([01])\.(\d)\.([A-H])$")
BLOCKS = ("ext", "test", "int", "mux", "load")
LOAD_PREFIX = "L"  # реле L1…L5 в файл данных плагина не входят (FW-242)
WORD_BITS = 32


class SourceError(ValueError):
    """Ошибка в relays.yaml: генерация невозможна."""


@dataclass(frozen=True)
class Relay:
    name: str
    number: int
    block: str
    address: str
    groups: tuple[str, ...]
    note: str = ""

    @property
    def is_load(self) -> bool:
        return self.name.startswith(LOAD_PREFIX) and self.name[1:].isdigit()


@dataclass(frozen=True)
class Group:
    name: str
    reason: str
    relays: tuple[Relay, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class Signal:
    name: str
    address: str
    dir: str
    pull: str
    init: int | None
    note: str = ""


@dataclass(frozen=True)
class Table:
    version: str
    relays: tuple[Relay, ...]
    groups: tuple[Group, ...]
    signals: tuple[Signal, ...] = ()

    @property
    def max_number(self) -> int:
        return max(r.number for r in self.relays)

    @property
    def words(self) -> int:
        return (self.max_number + WORD_BITS - 1) // WORD_BITS

    def canonical(self) -> str:
        body = {
            "table_version": self.version,
            "relays": [
                {
                    "name": r.name,
                    "number": r.number,
                    "block": r.block,
                    "address": r.address,
                    "groups": list(r.groups),
                }
                for r in self.relays
            ],
        }
        return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=True)

    def checksum(self) -> str:
        return "sha256:" + hashlib.sha256(self.canonical().encode("ascii")).hexdigest()


# ---------------------------------------------------------------- разбор источника


def load_table(path: Path) -> Table:
    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise SourceError("корень файла должен быть отображением")
    version = raw.get("table_version")
    if not isinstance(version, str) or not version:
        raise SourceError("table_version: непустая строка")
    next_number = raw.get("next_number")
    if not isinstance(next_number, int) or next_number < 1:
        raise SourceError("next_number: целое >= 1")
    retired = raw.get("retired", [])
    if not isinstance(retired, list) or any(not isinstance(n, int) for n in retired):
        raise SourceError("retired: список целых")

    group_defs = raw.get("groups")
    if not isinstance(group_defs, list):
        raise SourceError("groups: список групп")
    group_names: list[str] = []
    reasons: dict[str, str] = {}
    for g in group_defs:
        if not isinstance(g, dict) or not isinstance(g.get("name"), str):
            raise SourceError(f"группа без имени: {g!r}")
        name = g["name"]
        if not GROUP_RE.match(name):
            raise SourceError(f"имя группы {name!r} не по шаблону {GROUP_RE.pattern}")
        if name in reasons:
            raise SourceError(f"группа {name} объявлена дважды")
        group_names.append(name)
        reasons[name] = str(g.get("reason", ""))

    entries = raw.get("relays")
    if not isinstance(entries, list) or not entries:
        raise SourceError("relays: непустой список")
    relays: list[Relay] = []
    seen_names: dict[str, int] = {}
    seen_numbers: dict[int, str] = {}
    for e in entries:
        if not isinstance(e, dict):
            raise SourceError(f"запись реле не отображение: {e!r}")
        name, number = e.get("name"), e.get("number")
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise SourceError(f"имя реле {name!r} не по шаблону {NAME_RE.pattern}")
        if not isinstance(number, int) or number < 1:
            raise SourceError(f"{name}: номер должен быть целым >= 1")
        if number in retired:
            raise SourceError(f"{name}: номер {number} выведен из обращения (FW-244)")
        if number >= next_number:
            raise SourceError(f"{name}: номер {number} не меньше next_number={next_number} (FW-244)")
        if number in seen_numbers:
            raise SourceError(f"{name}: номер {number} уже занят реле {seen_numbers[number]} (FW-244)")
        key = name.upper()
        if key in seen_names:
            raise SourceError(f"имя {name} повторяется (без учёта регистра)")
        block = e.get("block")
        if block not in BLOCKS:
            raise SourceError(f"{name}: блок {block!r} не из {BLOCKS}")
        address = e.get("address")
        if not isinstance(address, str) or not (PIN_RE.match(address) or SR_RE.match(address)):
            raise SourceError(f"{name}: адрес {address!r} не вывод STM32 и не SRg.r.o")
        groups = e.get("groups", [])
        if not isinstance(groups, list) or any(not isinstance(g, str) for g in groups):
            raise SourceError(f"{name}: groups — список имён групп")
        for g in groups:
            if g not in reasons:
                raise SourceError(f"{name}: группа {g} не объявлена в groups")
        if len(set(groups)) != len(groups):
            raise SourceError(f"{name}: группа указана дважды")
        seen_names[key] = number
        seen_numbers[number] = name
        relays.append(Relay(name, number, block, address, tuple(groups), str(e.get("note", ""))))

    addresses: dict[str, str] = {}
    for r in relays:
        if r.address in addresses:
            raise SourceError(f"{r.name}: адрес {r.address} уже занят реле {addresses[r.address]}")
        addresses[r.address] = r.name

    groups: list[Group] = []
    for gname in group_names:
        members = tuple(r for r in relays if gname in r.groups)
        if len(members) < 2:
            raise SourceError(f"группа {gname}: меньше двух реле")
        groups.append(Group(gname, reasons[gname], members))
    return Table(version, tuple(relays), tuple(groups))


def load_signals(path: Path, relays: tuple[Relay, ...]) -> tuple[Signal, ...]:
    """Сигналы на выводах STM32, кроме реле; имена и адреса не пересекаются с реле."""
    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    entries = raw.get("signals") if isinstance(raw, dict) else None
    if not isinstance(entries, list) or not entries:
        raise SourceError("signals: непустой список")
    taken_names = {r.name.upper(): r.name for r in relays}
    taken_addr = {r.address: r.name for r in relays}
    out: list[Signal] = []
    for e in entries:
        if not isinstance(e, dict):
            raise SourceError(f"запись сигнала не отображение: {e!r}")
        name, address = e.get("name"), e.get("address")
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise SourceError(f"имя сигнала {name!r} не по шаблону {NAME_RE.pattern}")
        if name.upper() in taken_names:
            raise SourceError(f"сигнал {name}: имя занято ({taken_names[name.upper()]})")
        if not isinstance(address, str) or not PIN_RE.match(address):
            raise SourceError(f"{name}: адрес {address!r} не вывод STM32")
        if address in taken_addr:
            raise SourceError(f"{name}: вывод {address} уже занят ({taken_addr[address]})")
        d, pull = e.get("dir"), e.get("pull", "none")
        if d not in SIGNAL_DIRS:
            raise SourceError(f"{name}: dir {d!r} не из {SIGNAL_DIRS}")
        if pull not in PULLS:
            raise SourceError(f"{name}: pull {pull!r} не из {PULLS}")
        init = e.get("init")
        if d == "out":
            if init not in (0, 1):
                raise SourceError(f"{name}: выходу нужен init 0|1")
        elif init is not None:
            raise SourceError(f"{name}: init только у выходов")
        taken_names[name.upper()] = name
        taken_addr[address] = name
        out.append(Signal(name, address, d, pull, init, str(e.get("note", ""))))
    return tuple(out)


def load_all(source: Path = DEFAULT_SOURCE, signals: Path = DEFAULT_SIGNALS) -> Table:
    table = load_table(source)
    return Table(table.version, table.relays, table.groups, load_signals(signals, table.relays))


# ---------------------------------------------------------------- генерация


def _c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _addr_fields(address: str) -> str:
    m = PIN_RE.match(address)
    if m:
        port = ord(m.group(1)) - ord("A")
        return f"{{RELAY_ADDR_PIN, {port}, {int(m.group(2))}, 0}}"
    m = SR_RE.match(address)
    assert m
    return f"{{RELAY_ADDR_SR, {m.group(1)}, {m.group(2)}, {ord(m.group(3)) - ord('A')}}}"


def _mask_words(table: Table, relays: tuple[Relay, ...]) -> str:
    words = [0] * table.words
    for r in relays:
        idx = r.number - 1
        words[idx // WORD_BITS] |= 1 << (idx % WORD_BITS)
    return "{" + ", ".join(f"0x{w:08x}u" for w in words) + "}"


def gen_header(table: Table) -> str:
    lines = [
        "/* Сгенерировано tools/gen_board.py из board/rev1/relays.yaml — не править руками (FW-239). */",
        "#ifndef GEN_RELAY_TABLE_H",
        "#define GEN_RELAY_TABLE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define RELAY_TABLE_VERSION {_c_str(table.version)}",
        f"#define RELAY_TABLE_CHECKSUM {_c_str(table.checksum())}",
        f"#define RELAY_COUNT {len(table.relays)}",
        f"#define RELAY_MAX_NUMBER {table.max_number}",
        f"#define RELAY_WORDS {table.words}",
        f"#define RELAY_GROUP_COUNT {len(table.groups)}",
        "",
        "/* Адрес реле: вывод MCU (a = порт 0=A…, b = номер вывода) или выход цепочки",
        " * сдвиговых регистров (a = цепочка, b = регистр от MCU, c = выход 0=A…7=H). */",
        "enum relay_addr_kind { RELAY_ADDR_PIN = 0, RELAY_ADDR_SR = 1 };",
        "",
        "struct relay_addr {",
        "    uint8_t kind;",
        "    uint8_t a;",
        "    uint8_t b;",
        "    uint8_t c;",
        "};",
        "",
        "struct relay_desc {",
        "    const char *name;      /* каноническое имя (FW-112) */",
        "    uint8_t number;        /* постоянный номер (FW-244) */",
        "    const char *block;     /* блок описания платы */",
        "    const char *address;   /* адрес как в описании платы */",
        "    struct relay_addr addr;",
        "    uint16_t group_mask;   /* бит i — членство в relay_groups[i] */",
        "};",
        "",
        "/* Битовая карта реле: бит (number - 1) в словах по 32 (архитектура А7). */",
        "struct relay_group {",
        "    const char *name;",
        "    uint32_t mask[RELAY_WORDS];",
        "};",
        "",
        "extern const struct relay_desc relay_table[RELAY_COUNT];",
        "extern const struct relay_group relay_groups[RELAY_GROUP_COUNT];",
        "",
        "#endif /* GEN_RELAY_TABLE_H */",
        "",
    ]
    return "\n".join(lines)


def gen_source(table: Table) -> str:
    gidx = {g.name: i for i, g in enumerate(table.groups)}
    lines = [
        "/* Сгенерировано tools/gen_board.py из board/rev1/relays.yaml — не править руками (FW-239). */",
        '#include "relay_table.h"',
        "",
        "const struct relay_desc relay_table[RELAY_COUNT] = {",
    ]
    for r in table.relays:
        mask = 0
        for g in r.groups:
            mask |= 1 << gidx[g]
        lines.append(
            f"    {{{_c_str(r.name)}, {r.number}, {_c_str(r.block)}, {_c_str(r.address)}, "
            f"{_addr_fields(r.address)}, 0x{mask:04x}u}},"
        )
    lines += ["};", "", "const struct relay_group relay_groups[RELAY_GROUP_COUNT] = {"]
    for g in table.groups:
        lines.append(f"    {{{_c_str(g.name)}, {_mask_words(table, g.relays)}}},")
    lines += ["};", ""]
    return "\n".join(lines)


def _sig_ident(name: str) -> str:
    return "SIG_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def gen_signal_header(table: Table) -> str:
    lines = [
        "/* Сгенерировано tools/gen_board.py из board/rev1/signals.yaml — не править руками (FW-239). */",
        "#ifndef GEN_SIGNAL_TABLE_H",
        "#define GEN_SIGNAL_TABLE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SIGNAL_COUNT {len(table.signals)}",
        "",
        "/* dir: выход, вход, линия изделия (вход с подтяжкой вниз, при команде — выход 1) */",
        "enum signal_dir { SIGNAL_OUT = 0, SIGNAL_IN = 1, SIGNAL_DUT = 2 };",
        "enum signal_pull { SIGNAL_PULL_NONE = 0, SIGNAL_PULL_UP = 1, SIGNAL_PULL_DOWN = 2 };",
        "",
        "enum signal_id {",
    ]
    for sg in table.signals:
        lines.append(f"    {_sig_ident(sg.name)},")
    lines += [
        "};",
        "",
        "struct signal_desc {",
        "    const char *name;",
        "    const char *address;",
        "    uint8_t port;   /* 0 = A */",
        "    uint8_t pin;",
        "    uint8_t dir;    /* enum signal_dir */",
        "    uint8_t pull;   /* enum signal_pull */",
        "    uint8_t init;   /* уровень выхода при старте; для входов 0 */",
        "};",
        "",
        "extern const struct signal_desc signal_table[SIGNAL_COUNT];",
        "",
        "#endif /* GEN_SIGNAL_TABLE_H */",
        "",
    ]
    return "\n".join(lines)


def gen_signal_source(table: Table) -> str:
    lines = [
        "/* Сгенерировано tools/gen_board.py из board/rev1/signals.yaml — не править руками (FW-239). */",
        '#include "signal_table.h"',
        "",
        "const struct signal_desc signal_table[SIGNAL_COUNT] = {",
    ]
    dirs = {"out": "SIGNAL_OUT", "in": "SIGNAL_IN", "dut": "SIGNAL_DUT"}
    pulls = {"none": "SIGNAL_PULL_NONE", "up": "SIGNAL_PULL_UP", "down": "SIGNAL_PULL_DOWN"}
    for sg in table.signals:
        m = PIN_RE.match(sg.address)
        assert m
        port, pin = ord(m.group(1)) - ord("A"), int(m.group(2))
        lines.append(
            f"    [{_sig_ident(sg.name)}] = {{{_c_str(sg.name)}, {_c_str(sg.address)}, {port}, {pin}, "
            f"{dirs[sg.dir]}, {pulls[sg.pull]}, {sg.init or 0}}},"
        )
    lines += ["};", ""]
    return "\n".join(lines)


def pair_name(group: str, first: Relay, second: Relay) -> str:
    """Имя пары по FW-126: <группа>__<реле 1>__<реле 2>, недопустимые символы → '_'."""
    a, b = sorted((first, second), key=lambda r: r.number)
    return "__".join(re.sub(r"[^A-Za-z0-9_]", "_", s) for s in (group, a.name, b.name))


def expand_pairs(group: Group) -> list[dict[str, object]]:
    """Попарная развёртка шинной группы по возрастанию номеров (FW-243)."""
    members = sorted(group.relays, key=lambda r: r.number)
    pairs: list[dict[str, object]] = []
    for i, a in enumerate(members):
        for b in members[i + 1 :]:
            pairs.append({"name": pair_name(group.name, a, b), "relays": [a.number, b.number]})
    return pairs


def gen_interlock_json(table: Table) -> dict[str, object]:
    groups: list[dict[str, object]] = []
    for g in table.groups:
        groups.extend(expand_pairs(g))
    relays = [r.number for r in table.relays if not r.is_load]
    if not relays:
        raise SourceError("список relays файла данных пуст (FW-242)")
    return {
        "table_version": table.version,
        "checksum": table.checksum(),
        "relays": relays,
        "groups": groups,
    }


def gen_numbers_json(table: Table) -> dict[str, object]:
    return {
        "table_version": table.version,
        "checksum": table.checksum(),
        "relays": [
            {
                "name": r.name,
                "number": r.number,
                "block": r.block,
                "address": r.address,
                "groups": list(r.groups),
            }
            for r in table.relays
        ],
        "groups": [{"name": g.name, "relays": [r.name for r in g.relays]} for g in table.groups],
        "signals": [
            {"name": sg.name, "address": sg.address, "dir": sg.dir, "pull": sg.pull, "init": sg.init}
            for sg in table.signals
        ],
    }


def gen_doc(table: Table) -> str:
    lines = [
        "<!-- Сгенерировано tools/gen_board.py из board/rev1/relays.yaml — не править руками (FW-239). -->",
        f"# Группы блокировок CONTR, таблица `{table.version}`",
        "",
        f"Контрольная сумма: `{table.checksum()}`. Реле: {len(table.relays)}, групп: {len(table.groups)}.",
        "",
        "| Группа | Реле | Основание |",
        "|---|---|---|",
    ]
    for g in table.groups:
        names = ", ".join(f"`{r.name}`" for r in g.relays)
        lines.append(f"| `{g.name}` | {names} | {g.reason} |")
    outside = [r.name for r in table.relays if not r.groups]
    lines += ["", "Вне групп: " + ", ".join(f"`{n}`" for n in outside) + ".", ""]
    return "\n".join(lines)


def render_all(table: Table) -> dict[str, str]:
    json_kw = {"ensure_ascii": False, "indent": 2}
    return {
        "relay_table.h": gen_header(table),
        "relay_table.c": gen_source(table),
        "signal_table.h": gen_signal_header(table),
        "signal_table.c": gen_signal_source(table),
        "interlock-table.json": json.dumps(gen_interlock_json(table), **json_kw) + "\n",
        "relay-numbers.json": json.dumps(gen_numbers_json(table), **json_kw) + "\n",
        "interlock-groups.md": gen_doc(table),
    }


def write_all(table: Table, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in render_all(table).items():
        (out_dir / name).write_text(text, encoding="utf-8", newline="\n")


def check_all(table: Table, out_dir: Path) -> list[str]:
    """Имена файлов gen/, отличающихся от свежей генерации (FW-240)."""
    stale: list[str] = []
    for name, text in render_all(table).items():
        path = out_dir / name
        if not path.exists() or path.read_text(encoding="utf-8").replace("\r\n", "\n") != text:
            stale.append(name)
    return stale


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--signals", type=Path, default=DEFAULT_SIGNALS)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true", help="только сверить gen/ с источником")
    args = parser.parse_args(argv)
    try:
        table = load_all(args.source, args.signals)
    except SourceError as exc:
        print(f"{args.source}: {exc}", file=sys.stderr)
        return 2
    if args.check:
        stale = check_all(table, args.out)
        if stale:
            print("gen/ расходится с источником, перегенерируйте: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"gen/ соответствует {args.source.name} ({table.checksum()})")
        return 0
    write_all(table, args.out)
    print(f"сгенерировано {len(table.relays)} реле, {len(table.groups)} групп, "
          f"{len(table.signals)} сигналов, {table.checksum()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
