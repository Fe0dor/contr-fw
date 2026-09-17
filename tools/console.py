#!/usr/bin/env python3
"""Консоль USART3 платы CONTR через виртуальный COM-порт отладчика (план 22, раздел 0).

Строчный терминал: каждая набранная строка уходит в плату с `\\n`, всё принятое
печатается построчно с меткой времени. Порт ищется автоматически по VID SEGGER
(J-Link OB) или ST (ST-LINK), иначе задаётся явно.

    python tools/console.py                 # автопоиск порта, 115200
    python tools/console.py --port COM5
    python tools/console.py --list          # показать доступные порты
    python tools/console.py --send "*IDN?"  # отправить одну строку, напечатать ответ, выйти
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from datetime import datetime

import serial
from serial.tools import list_ports

def _utf8_console() -> None:
    """Терминал Windows по умолчанию в cp1251/cp866: русские сообщения не должны ронять скрипт."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_utf8_console()

BAUD = 115200
KNOWN_VIDS = {0x1366: "SEGGER J-Link", 0x0483: "ST-LINK"}


def list_candidates() -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for p in list_ports.comports():
        label = KNOWN_VIDS.get(p.vid or -1)
        if label:
            found.append((p.device, f"{label}: {p.description}"))
    return found


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    cands = list_candidates()
    if len(cands) == 1:
        return cands[0][0]
    if not cands:
        raise SystemExit("порт не найден: подключите плату или укажите --port")
    raise SystemExit("несколько портов, укажите --port: " + ", ".join(f"{d} ({l})" for d, l in cands))


def stamp() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def reader(port: serial.Serial, stop: threading.Event) -> None:
    buf = bytearray()
    while not stop.is_set():
        chunk = port.read(256)
        if not chunk:
            continue
        buf.extend(chunk)
        while b"\n" in buf:
            line, _, rest = buf.partition(b"\n")
            buf[:] = rest
            print(f"{stamp()} < {line.decode('ascii', 'replace').rstrip()}", flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="COM-порт / /dev/tty.*; по умолчанию автопоиск")
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--list", action="store_true", help="показать порты отладчиков и выйти")
    parser.add_argument("--send", metavar="LINE", help="отправить одну строку и выйти через --wait секунд")
    parser.add_argument("--wait", type=float, default=1.0, help="ожидание ответа для --send, с")
    args = parser.parse_args(argv)

    if args.list:
        for dev, label in list_candidates():
            print(f"{dev}\t{label}")
        return 0

    port_name = pick_port(args.port)
    with serial.Serial(port_name, args.baud, timeout=0.05) as port:
        print(f"{stamp()} = {port_name} @ {args.baud}, Ctrl+C — выход", flush=True)
        stop = threading.Event()
        t = threading.Thread(target=reader, args=(port, stop), daemon=True)
        t.start()
        try:
            if args.send is not None:
                port.write(args.send.encode("ascii") + b"\n")
                print(f"{stamp()} > {args.send}", flush=True)
                time.sleep(args.wait)
                return 0
            for line in sys.stdin:
                text = line.rstrip("\r\n")
                port.write(text.encode("ascii", "replace") + b"\n")
                print(f"{stamp()} > {text}", flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
