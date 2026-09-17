#!/usr/bin/env python3
"""Прогонщик эталонных диалогов (А13): один корпус, два исполнителя — хост-сборка прошивки
(contr-host) и эмулятор (contr_emu), оба по TCP на 127.0.0.1.

    python tools/run_dialogs.py --host build/host/contr-host      # против хост-сборки
    python tools/run_dialogs.py --emu                             # против эмулятора
    python tools/run_dialogs.py --tcp 192.168.0.20:5025           # против платы
    python tools/run_dialogs.py --emu dialogs/step2-protocol.txt  # один файл

Формат файла dialogs/*.txt:
    # комментарий
    > КОМАНДА                  отправить по TCP
    < ответ                    ожидаемый ответ целиком (префикс G<n>; сравнивается тоже)
    <~ регулярное выражение    ожидаемый ответ по re.fullmatch
    {G}, {G+n}                 в ожидании: поколение из последнего ответа и на n больше;
                               пока поколение неизвестно, {G} — любое число
    #advance 500               продвинуть виртуальное время исполнителя на 500 мс
    #reconnect                 закрыть соединение и открыть новое (новый клиент)
    #disconnect / #connect     закрыть соединение без клиента / подключиться снова
    #second-client < ERR:BUSY  открыть второе соединение и проверить его первый ответ
    #console > КОМАНДА         только у исполнителей с консолью (contr-host: stdin) — здесь
                               пропускается, если консоли нет
Исполнение останавливается на первом расхождении в файле; код возврата 1.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIALOGS = ROOT / "dialogs"


class Target:
    def __init__(self, host: str, port: int, console: subprocess.Popen | None = None) -> None:
        self.host, self.port = host, port
        self.console = console
        self.sock: socket.socket | None = None
        self.buf = b""

    def connect(self, timeout: float = 5.0) -> None:
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=2)
                self.sock.settimeout(5)
                self.buf = b""
                return
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None
        time.sleep(0.1)

    def send(self, line: str) -> None:
        assert self.sock
        self.sock.sendall((line + "\n").encode("ascii"))

    def recv_line(self) -> str:
        assert self.sock
        while b"\n" not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("соединение закрыто")
            self.buf += data
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("ascii", "replace").rstrip("\r")

    def advance(self, ms: int) -> None:
        # contr-host и эмулятор понимают директиву на любом канале
        self.send(f"#advance {ms}")

    def console_send(self, line: str) -> str | None:
        if not self.console or not self.console.stdin or not self.console.stdout:
            return None
        self.console.stdin.write(line + "\n")
        self.console.stdin.flush()
        return self.console.stdout.readline().rstrip("\r\n")


GEN_RE = re.compile(r"^G(\d+);")


PLACEHOLDER_RE = re.compile(r"\{G(?:\+(\d+))?\}")


def expand(expected: str, gen: int | None, regex: bool) -> str:
    """Подстановка {G} и {G+n}; при неизвестном поколении — любое число."""
    del regex

    def sub(m: re.Match) -> str:
        if gen is None:
            return r"\d+"
        return str(gen + int(m.group(1) or 0))

    return PLACEHOLDER_RE.sub(sub, expected)


def run_file(path: Path, target: Target) -> tuple[int, str | None]:
    """(число проверок, текст первой ошибки или None)."""
    try:
        return _run_file(path, target)
    finally:
        target.close()


def _run_file(path: Path, target: Target) -> tuple[int, str | None]:
    checks = 0
    gen: int | None = None
    target.connect()
    pending_console: str | None = None
    for lineno, raw in enumerate(path.read_text("utf-8").splitlines(), 1):
        line = raw.strip()
        directives = ("#advance", "#reconnect", "#disconnect", "#connect", "#second-client", "#console")
        if not line or (line.startswith("#") and not line.startswith(directives)):
            continue
        where = f"{path.name}:{lineno}"
        if line.startswith("#advance"):
            target.advance(int(line.split()[1]))
        elif line == "#reconnect":
            target.close()
            target.connect()
        elif line == "#disconnect":
            target.close()
        elif line == "#connect":
            target.connect()
        elif line.startswith("#second-client"):
            expected = line.split("<", 1)[1].strip()
            with socket.create_connection((target.host, target.port), timeout=2) as second:
                second.settimeout(3)
                got = second.recv(64).decode("ascii", "replace").strip()
            checks += 1
            if got != expected:
                return checks, f"{where}: второй клиент: ожидалось {expected!r}, получено {got!r}"
        elif line.startswith("#console"):
            if target.console is None:
                return checks, "SKIP: нет консольного канала; файл проверен лишь частично"
            cmd = line.split(">", 1)[1].strip()
            pending_console = target.console_send(cmd)
        elif line.startswith(">"):
            target.send(line[1:].strip())
        elif line.startswith("<~") or line.startswith("<"):
            regex = line.startswith("<~")
            expected = line[2:].strip() if regex else line[1:].strip()
            if pending_console is not None:
                got = pending_console
                pending_console = None
            else:
                got = target.recv_line()
            checks += 1
            unknown_gen = gen is None and "{G" in expected
            expected = expand(expected, gen, regex)
            if regex or unknown_gen:
                ok = re.fullmatch(expected if regex else re.escape(expected).replace(r"\d\+", r"\d+"), got) is not None
            else:
                ok = got == expected
            m = GEN_RE.match(got)
            if m:
                gen = int(m.group(1))
            if not ok:
                return checks, f"{where}: ожидалось {expected!r}, получено {got!r}"
        else:
            return checks, f"{where}: непонятная строка {raw!r}"
    return checks, None


def start_host(exe: Path, port: int) -> subprocess.Popen:
    env = dict(os.environ, CONTR_HOST_PORT=str(port))
    return subprocess.Popen([str(exe)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)


def start_emu(port: int) -> subprocess.Popen:
    env = dict(os.environ, PYTHONPATH=str(ROOT / "emu") + os.pathsep + os.environ.get("PYTHONPATH", ""))
    return subprocess.Popen([sys.executable, "-m", "contr_emu", "--port", str(port), "--console"], env=env,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            bufsize=1)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--host", type=Path, help="исполняемый файл contr-host")
    group.add_argument("--emu", action="store_true", help="эмулятор contr_emu из репозитория")
    group.add_argument("--tcp", help="адрес:порт готового устройства или сервера")
    parser.add_argument("--port", type=int, default=15025, help="порт для запускаемого процесса")
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)

    files = args.files or sorted(DIALOGS.glob("*.txt"))
    proc: subprocess.Popen | None = None
    if args.host:
        proc = start_host(args.host, args.port)
        target = Target("127.0.0.1", args.port, proc)
    elif args.emu:
        proc = start_emu(args.port)
        target = Target("127.0.0.1", args.port, proc)
    else:
        host, _, port = args.tcp.partition(":")
        target = Target(host, int(port or 5025))

    failed = 0
    skipped = 0
    total = 0
    try:
        for path in files:
            try:
                checks, error = run_file(path, target)
            except (OSError, ConnectionError) as exc:
                checks, error = 0, f"{path.name}: {exc}"
            total += checks
            skip = error is not None and error.startswith("SKIP:")
            status = "SKIP" if skip else ("OK" if error is None else "FAIL")
            print(f"{status:4} {path.name}: {checks} проверок" + (f" — {error}" if error else ""))
            failed += error is not None and not skip
            skipped += skip
            if proc is not None and proc.poll() is not None:
                print(f"исполнитель завершился с кодом {proc.returncode}")
                # перезапуск, если сам процесс вышел (например, после сброса)
                proc = start_host(args.host, args.port) if args.host else start_emu(args.port)
                target.console = proc
    finally:
        if proc is not None:
            proc.kill()
            proc.wait(timeout=5)
    print(f"итого: {len(files)} файлов, {total} проверок, {failed} с ошибками, {skipped} пропущено")
    return 1 if failed or skipped else 0


if __name__ == "__main__":
    sys.exit(main())
