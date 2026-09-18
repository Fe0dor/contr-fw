"""Модель устройства CONTR для эмулятора (FW-248…FW-250): протокол шага 2 плана 22.

Тот же набор команд, что в прошивке шага 2, те же коды ошибок, префикс поколения по
TCP, арбитраж консоли и клиента, таблица реле и блокировок из gen/relay-numbers.json
и gen/interlock-table.json (версия и сумма те же, что в файле данных плагина).
Виртуальное время продвигается директивой ``#advance <мс>`` или advance().
"""

from __future__ import annotations

import base64
import json
import zlib
from dataclasses import dataclass, field
from pathlib import Path

GEN = Path(__file__).resolve().parents[2] / "gen"
VERSION = "0.2.0"
VENDOR = "TESTDUT"
MODEL = "CONTR"
BOARD_REV = 1
UPD_BLOCK_MAX_B = 256
UPD_CONFIRM_TIMEOUT_MS = 60000
UPD_MIN_VERSION = (0, 2, 0)
DHCP_TIMEOUT_MS = 10000
NET_DEFAULT = ("static", "192.168.0.20", "255.255.255.0", "192.168.0.100")
LOG_DEPTH = 256
IMAGE_MAX = (1024 - 128) * 1024

CONFIG = [
    ("BOARD_REV", "1"), ("PWRON_AB_ON_LEVEL", "1"), ("PWROK_PRESENT", "0"), ("PWROK_ACTIVE_LOW", "1"),
    ("PWROK_AB_ACTIVE_LOW", "1"), ("CS_LOAD_ON_SR", "1"), ("CS_ACTIVE_LOW", "1"), ("PWRON_RSP_ON_LEVEL", "0"),
    ("SR1_LOOP_PRESENT", "0"), ("RELAY_BREAK_MS", "20"), ("PSU_VMIN", "42"), ("PSU_VMAX", "52"),
    ("PSU_DAC_MIN", "0"), ("PSU_DAC_MAX", "4095"), ("DCOK_ACTIVE_HIGH", "1"), ("ALARM_ACTIVE_HIGH", "1"),
    ("DCOK_RISE_TIMEOUT_MS", "2000"), ("DCOK_FALL_TIMEOUT_MS", "2000"), ("SAFE_STATE_BUDGET_MS", "4000"),
    ("TCP_KEEPALIVE_IDLE_S", "10"), ("TCP_KEEPALIVE_INTVL_S", "5"), ("TCP_KEEPALIVE_CNT", "4"),
    ("DUT_POLL_MS", "10"), ("DUT_DEBOUNCE_MS", "30"), ("I2C_RETRIES", "3"), ("I2C_SPEED_HZ", "400000"),
    ("SECTION_PWROK_TIMEOUT_MS", "500"), ("SECTION_SETTLE_MS", "100"), ("SECTION_ON_MAX_MS", "1000"),
    ("LOAD_HEARTBEAT_MS", "100"), ("LOAD_LINK_TIMEOUT_MS", "500"), ("LOAD_LINK_MARGIN_MS", "100"),
    ("LOAD_FRAME_GAP_US", "100"), ("LOAD_PROTO_MIN", "1"), ("LOAD_PROTO_MAX", "1"), ("PWM_FREQ_MIN_HZ", "1000"),
    ("PWM_FREQ_MAX_HZ", "100000"), ("PWM_FREQ_STEP_HZ", "1000"), ("DHCP_TIMEOUT_MS", "10000"),
    ("NET_DEFAULT_ADDR", "192.168.0.20"), ("NET_DEFAULT_MASK", "255.255.255.0"), ("NET_DEFAULT_GW", "192.168.0.100"),
    ("BUTTON_RESET_HOLD_MS", "5000"), ("UPD_CONFIRM_TIMEOUT_MS", "60000"), ("UPD_BLOCK_MAX_B", "256"),
    ("UPD_MIN_VERSION", "0.2.0"), ("LOG_DEPTH", "256"), ("OPT_NDBANK", "0"), ("OPT_NDBOOT", "0"), ("OPT_IWDG_SW", "0"),
]


class CommandError(Exception):
    def __init__(self, code: str, fields: str = "") -> None:
        super().__init__(code)
        self.code = code
        self.fields = fields


def load_tables() -> tuple[dict, dict]:
    numbers = json.loads((GEN / "relay-numbers.json").read_text("utf-8"))
    plugin = json.loads((GEN / "interlock-table.json").read_text("utf-8"))
    return numbers, plugin


@dataclass
class SafeReport:
    seq: int = 0
    reason: str = "RESET"
    steps: list[str] = field(default_factory=lambda: ["OK", "SILENT", "OK", "OK", "OK", "OK"])
    duration_ms: int = 0

    def text(self) -> str:
        return f"{self.seq},{self.reason}," + ",".join(self.steps) + f",{self.duration_ms}"


class Device:
    """Состояние и команды. Каналы: 'tcp' (с префиксом G<n>;) и 'console'."""

    def __init__(self) -> None:
        self.numbers, self.plugin = load_tables()
        self.relays = {r["name"]: r for r in self.numbers["relays"]}
        self.now_ms = 0
        self.generation = 0
        self.client_connected = False
        self.in_safe_state = True
        self.safe = SafeReport()
        self.errors: list[tuple[str, str]] = []
        self.log: list[tuple[int, str]] = []
        self.serial: str | None = None
        self.net = NET_DEFAULT
        self.net_pending = NET_DEFAULT
        self.dhcp_fallback = False
        self.relays_on: set[str] = set()
        self.upd_state = "IDLE"
        self.upd_expected = (0, 0)
        self.upd_buf = bytearray()
        self.trial_bank: int | None = None
        self.trial_started = 0
        self.trial_confirmed = False
        self.trial_boot_count = 0
        self.active_bank = 1
        self.bank_versions = {1: VERSION, 2: VERSION}
        self.boot_bank = 1
        self.optbytes_ok = True
        self.reset_cause = "POWER"
        self.log_event("reset: power-on")

    # ---- служебное ----

    def advance(self, ms: int) -> None:
        self.now_ms += ms
        if self.upd_state == "TRIAL" and self.now_ms - self.trial_started >= UPD_CONFIRM_TIMEOUT_MS:
            self.log_event("update: confirm timeout, rollback")
            self.boot_bank = 1 if self.active_bank == 2 else 2
            self.reboot()
        if self.net[0] == "dhcp" and not self.dhcp_fallback and self.now_ms >= DHCP_TIMEOUT_MS:
            self.dhcp_fallback = True
            self.log_event("DHCP timeout, static 192.168.0.20")

    def log_event(self, text: str) -> None:
        self.log.append((self.now_ms, text[:39]))
        del self.log[:-LOG_DEPTH]

    def push_error(self, code: str, text: str = "") -> None:
        self.errors.append((code, text))
        del self.errors[:-16]

    def safe_enter(self, reason: str) -> bool:
        self.safe = SafeReport(self.safe.seq + 1, reason, ["OK", "SILENT", "OK", "OK", "OK", "OK"], 0)
        self.relays_on.clear()
        self.in_safe_state = True
        self.generation += 1
        self.log_event(f"SAFE {reason} OK 0 ms")
        return True

    def connect(self) -> bool:
        """Новое TCP-соединение: False — ERR:BUSY (второй клиент)."""
        if self.client_connected:
            return False
        self.client_connected = True
        self.log_event("client connected")
        self.safe_enter("CONNECT")
        return True

    def disconnect(self) -> None:
        if self.client_connected:
            self.client_connected = False
            self.log_event("client lost")
            self.safe_enter("CLIENT_LOST")

    def reboot(self) -> None:
        """Сброс: сеть по записи, пробный запуск по записи, поколение и состояние заново."""
        self.client_connected = False
        self.active_bank = self.boot_bank
        self.net = self.net_pending
        self.dhcp_fallback = False
        self.now_ms = 0
        self.reset_cause = "SOFTWARE"
        self.log_event("reset: software")
        if self.trial_bank == self.active_bank and not self.trial_confirmed and self.trial_boot_count:
            self.boot_bank = 1 if self.active_bank == 2 else 2
            self.active_bank = self.boot_bank
        if self.trial_bank == self.active_bank and not self.trial_confirmed:
            self.trial_boot_count += 1
            self.upd_state = "TRIAL"
            self.trial_started = self.now_ms
            self.log_event(f"update: trial run bank {self.active_bank}")
        else:
            self.upd_state = "IDLE"
            if self.trial_bank is not None and self.trial_bank != self.active_bank and not self.trial_confirmed:
                self.push_error("UPD_SEQ", "update rolled back")
                self.trial_bank = None
        self.generation = 0
        self.in_safe_state = True
        self.safe = SafeReport(0, "RESET")

    # ---- исполнение ----

    def execute(self, channel: str, line: str) -> str | None:
        """Одна строка → один кадр ответа без '\\n'; None для пустой строки."""
        line = line.strip("\r\n")
        if line.startswith("#advance"):
            self.advance(int(line.split()[1]))
            return None
        if not line.strip():
            return None
        name, _, rest = line.strip().partition(" ")
        name = name.upper()
        args = [a for a in rest.strip().split(",")] if rest.strip() else []
        try:
            body = self._dispatch(channel, name, args)
        except CommandError as exc:
            self.push_error(exc.code, exc.fields)
            self.log_event(f"{name} ERR:{exc.code}")
            body = f"ERR:{exc.code}" + (f",{exc.fields}" if exc.fields else "")
        # префикс — поколение на момент ответа, как в прошивке (после SAFE уже новое)
        prefix = f"G{self.generation};" if channel == "tcp" else ""
        return prefix + body

    QUERIES = {"*IDN?", "SYST:SAFE?", "SYST:ERR?", "SYST:LOG?", "SYST:CONF?", "SYST:NET?", "SYST:UPD:STAT?",
               "TEST:ALL?", "INTERLOCK:LIST?"}
    CONSOLE_ONLY = {"SYST:PROV:SERIAL", "SYST:PROV:NET"}
    SETS = {"SYST:UPD:BEGIN", "SYST:UPD:DATA", "SYST:UPD:COMMIT", "SYST:UPD:ABORT", "SYST:UPD:CONFIRM"}
    ARGS = {"*IDN?": (0, 0), "SAFE": (0, 0), "SYST:SAFE?": (0, 0), "SYST:ERR?": (0, 0), "SYST:LOG?": (0, 0),
            "SYST:CONF?": (0, 0), "SYST:NET?": (0, 0), "SYST:PROV:SERIAL": (1, 1), "SYST:PROV:NET": (1, 4),
            "SYST:UPD:BEGIN": (3, 3), "SYST:UPD:DATA": (1, 1), "SYST:UPD:COMMIT": (0, 0), "SYST:UPD:ABORT": (0, 0),
            "SYST:UPD:STAT?": (0, 0), "SYST:UPD:CONFIRM": (0, 0), "TEST:ALL?": (0, 0), "INTERLOCK:LIST?": (0, 0)}

    def _dispatch(self, channel: str, name: str, args: list[str]) -> str:
        if name not in self.ARGS:
            raise CommandError("UNKNOWN_CMD")
        lo, hi = self.ARGS[name]
        if not lo <= len(args) <= hi:
            raise CommandError("RANGE", "args")
        if name in self.CONSOLE_ONLY:
            if channel != "console" or self.client_connected:
                raise CommandError("CONSOLE_ONLY")
        elif channel == "console" and self.client_connected and name not in self.QUERIES and name != "SAFE":
            raise CommandError("BUSY")
        handler = getattr(self, "cmd_" + name.replace("*", "star").replace(":", "_").replace("?", "_q").lower())
        result = handler(args)
        if name in self.SETS and not name.endswith("?"):
            self.in_safe_state = False if name != "SYST:UPD:ABORT" else self.in_safe_state
        if channel == "console" and name not in self.QUERIES and name != "SAFE" and name not in self.CONSOLE_ONLY:
            self.generation += 1
        return result

    # ---- команды ----

    def cmd_staridn_q(self, args: list[str]) -> str:
        return f"{VENDOR},{MODEL}-R{BOARD_REV},{self.serial or 'UNPROVISIONED'},{self.bank_versions[self.active_bank]}"

    def cmd_safe(self, args: list[str]) -> str:
        self.safe_enter("SAFE")
        return "OK"

    def cmd_syst_safe_q(self, args: list[str]) -> str:
        return self.safe.text()

    def cmd_syst_err_q(self, args: list[str]) -> str:
        if not self.errors:
            return "NONE"
        items = [code + (f",{text}" if text else "") for code, text in self.errors]
        self.errors.clear()
        return ";".join(items)

    def cmd_syst_log_q(self, args: list[str]) -> str:
        return ";".join(f"{ms},{text}" for ms, text in self.log) or "NONE"

    def cmd_syst_conf_q(self, args: list[str]) -> str:
        return ";".join(f"{k}={v}" for k, v in CONFIG)

    def mode_name(self) -> str:
        if self.net[0] == "static":
            return "static"
        return "dhcp-fallback" if self.dhcp_fallback else "dhcp"

    def current_ip(self) -> str:
        if self.net[0] == "static" or self.dhcp_fallback:
            return self.net[1] if self.net[0] == "static" else NET_DEFAULT[1]
        return "0.0.0.0"

    def cmd_syst_net_q(self, args: list[str]) -> str:
        return f"{self.mode_name()},02:EE:00:00:00:01,{self.current_ip()},UP"

    def cmd_syst_prov_serial(self, args: list[str]) -> str:
        if self.serial is not None:
            raise CommandError("PROVISIONED")
        s = args[0]
        if not s or len(s) > 32 or not all(ch.isalnum() or ch in "-_" for ch in s):
            raise CommandError("RANGE", "serial")
        self.serial = s
        self.log_event(f"provisioned {s}")
        return "OK"

    @staticmethod
    def _valid_ip(s: str) -> bool:
        parts = s.split(".")
        return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 and len(p) <= 3 for p in parts)

    def cmd_syst_prov_net(self, args: list[str]) -> str:
        mode = args[0].lower()
        if mode == "dhcp" and len(args) == 1:
            self.net_pending = ("dhcp", NET_DEFAULT[1], NET_DEFAULT[2], NET_DEFAULT[3])
        elif mode == "static" and len(args) == 4 and all(self._valid_ip(a) for a in args[1:]):
            self.net_pending = ("static", args[1], args[2], args[3])
        else:
            raise CommandError("RANGE", "net")
        self.log_event(f"net {mode}")
        return "OK"

    def cmd_syst_upd_begin(self, args: list[str]) -> str:
        if self.upd_state == "TRIAL":
            raise CommandError("UPD_SEQ", "trial pending")
        if not self.in_safe_state:
            raise CommandError("NOT_SAFE", "STATE")
        if not self.optbytes_ok:
            raise CommandError("NOT_SAFE", "OPTBYTES")
        try:
            size = int(args[0])
            crc = int(args[1], 16)
            ver = tuple(int(x) for x in args[2].split("."))
            if len(ver) != 3 or any(v < 0 or v > 255 for v in ver) or not 0 <= crc <= 0xFFFFFFFF:
                raise ValueError
        except ValueError:
            raise CommandError("RANGE", "args") from None
        if size < 0x200 + 36 or size > IMAGE_MAX:
            raise CommandError("RANGE", "size")
        if ver < UPD_MIN_VERSION:
            raise CommandError("UPD_HEADER", "version")
        self.upd_state = "RECEIVING"
        self.upd_expected = (size, crc)
        self.upd_buf = bytearray()
        return "OK"

    def cmd_syst_upd_data(self, args: list[str]) -> str:
        if self.upd_state != "RECEIVING":
            raise CommandError("UPD_SEQ")
        try:
            block = base64.b64decode(args[0], validate=True)
        except Exception:
            raise CommandError("RANGE", "block") from None
        if len(block) > UPD_BLOCK_MAX_B:
            raise CommandError("RANGE", "block")
        if len(self.upd_buf) + len(block) > self.upd_expected[0]:
            raise CommandError("RANGE", "size")
        self.upd_buf += block
        return "OK"

    def cmd_syst_upd_commit(self, args: list[str]) -> str:
        if self.upd_state != "RECEIVING":
            raise CommandError("UPD_SEQ")
        size, crc = self.upd_expected
        img = bytes(self.upd_buf)
        if len(img) != size:
            self.upd_state = "IDLE"
            raise CommandError("UPD_CRC", "size")
        if zlib.crc32(img) & 0xFFFFFFFF != crc:
            self.upd_state = "IDLE"
            raise CommandError("UPD_CRC")
        hdr = img[0x200:0x200 + 36]
        if hdr[:8] != b"CONTRFW1":
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "magic")
        if hdr[8:16].rstrip(b"\0") != MODEL.encode():
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "model")
        if hdr[16] != BOARD_REV:
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "revision")
        if int.from_bytes(hdr[20:24], "little") != size:
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "size")
        if tuple(hdr[17:20]) < UPD_MIN_VERSION:
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "version")
        sp = int.from_bytes(img[:4], "little")
        reset = int.from_bytes(img[4:8], "little")
        if not (0x20000000 < sp <= 0x20080000 and sp % 8 == 0
                and reset & 1 and 0x08000224 <= (reset & ~1) <= 0x08000000 + len(img) - 2):
            self.upd_state = "IDLE"
            raise CommandError("UPD_HEADER", "vectors")
        target = 2 if self.active_bank == 1 else 1
        self.bank_versions[target] = ".".join(str(v) for v in hdr[17:20])
        self.trial_bank = target
        self.trial_confirmed = False
        self.trial_boot_count = 0
        self.boot_bank = target
        self.log_event(f"update: commit to bank {target}")
        self.upd_state = "COMMITTED"  # сервер делает reboot() после ответа
        return "OK"

    def cmd_syst_upd_abort(self, args: list[str]) -> str:
        if self.upd_state == "RECEIVING":
            self.upd_state = "IDLE"
        return "OK"

    def cmd_syst_upd_stat_q(self, args: list[str]) -> str:
        expected = self.upd_expected[0] if self.upd_state == "RECEIVING" else 0
        received = len(self.upd_buf) if self.upd_state == "RECEIVING" else 0
        return f"{self.upd_state},{received},{expected}"

    def cmd_syst_upd_confirm(self, args: list[str]) -> str:
        if self.upd_state != "TRIAL":
            raise CommandError("UPD_SEQ")
        self.trial_confirmed = True
        self.upd_state = "CONFIRMED"
        self.log_event("update: confirmed")
        return "OK"

    def cmd_test_all_q(self, args: list[str]) -> str:
        verdict = "FAIL" if not self.optbytes_ok else "WARN"
        return (f"{verdict};SR0:UNTESTED;SR1:UNVERIFIED;I2C_A:SKIP,OFF;I2C_B:SKIP,OFF;DCOK:1;ALARM:1;"
                f"LOADBOARD:LINK_LOST;INTERLOCK:{self.plugin['checksum']};PROV:{self.serial or 'UNPROVISIONED'};"
                f"OPTBYTES:{'OK' if self.optbytes_ok else 'MISMATCH'};NET:{self.mode_name()},{self.current_ip()},UP;"
                f"RESET:{self.reset_cause};SAFE:SILENT")

    def cmd_interlock_list_q(self, args: list[str]) -> str:
        records = [f"{r['name']},{r['number']},{r['block']},{r['address']},{'+'.join(r['groups'])}"
                   for r in self.numbers["relays"]]
        records.append(f"{self.plugin['table_version']},{self.plugin['checksum']}")
        return ";".join(records)
