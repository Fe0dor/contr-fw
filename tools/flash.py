#!/usr/bin/env python3
"""Загрузка образа в плату CONTR через SEGGER J-Link (план 22, раздел 0).

Запускает J-Link Commander (JLink.exe на Windows, JLinkExe на Mac/Linux) с файлом
команд: подключение по SWD, загрузка hex, сброс, запуск. Отладчик NUCLEO должен быть
перепрошит в J-Link OB, а SEGGER J-Link Software установлен (README, «Установка»).

    python tools/flash.py                       # build/target/contr-fw.hex
    python tools/flash.py path/to/image.hex
    python tools/flash.py --jlink "C:/Program Files/SEGGER/JLink/JLink.exe"
    python tools/flash.py --reset-only          # только сброс и запуск
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

def _utf8_console() -> None:
    """Терминал Windows по умолчанию в cp1251/cp866: русские сообщения не должны ронять скрипт."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_utf8_console()

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = ROOT / "build" / "target" / "contr-fw.hex"
DEVICE = "STM32F767ZI"
SPEED_KHZ = 1000

CANDIDATES = (
    "JLink.exe",
    "JLinkExe",
    r"C:\Program Files\SEGGER\JLink\JLink.exe",
    r"C:\Program Files (x86)\SEGGER\JLink\JLink.exe",
    "/Applications/SEGGER/JLink/JLinkExe",
    "/opt/SEGGER/JLink/JLinkExe",
)


def find_jlink(explicit: str | None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("JLINK_EXE")
    if env:
        return env
    for cand in CANDIDATES:
        found = shutil.which(cand) if os.sep not in cand else (cand if Path(cand).exists() else None)
        if found:
            return found
    # установщик SEGGER кладёт версию в имя каталога: JLink_V818; берём самую свежую
    for pattern in (r"C:\Program Files\SEGGER\JLink*\JLink.exe", "/Applications/SEGGER/JLink*/JLinkExe"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[-1]
    raise FileNotFoundError(
        "J-Link Commander не найден: установите SEGGER J-Link Software, "
        "добавьте его в PATH или задайте JLINK_EXE / --jlink"
    )


def build_script(image: Path | None, loader_dir: Path | None = None) -> str:
    lines = []
    if loader_dir is not None:
        lines.append(f"exec JLinkDevicesXMLPath = {loader_dir.resolve().as_posix()}/")
    lines += ["R0", "Sleep 100", "connect", "R1", "h", "w4 0xE0042008 0x00001000"]
    if image is not None:
        lines.append(f"loadfile {image.as_posix()}")
    lines += ["r", "g", "q", ""]
    return "\n".join(lines)


def run(jlink: str, script: str) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False, encoding="ascii") as f:
        f.write(script)
        script_path = f.name
    cmd = [
        jlink,
        "-Device", DEVICE,
        "-If", "SWD",
        "-Speed", str(SPEED_KHZ),
        "-AutoConnect", "0",
        "-NoGui", "1",
        "-ExitOnError", "1",
        "-CommandFile", script_path,
    ]
    print("$", " ".join(cmd), flush=True)
    try:
        return subprocess.call(cmd)
    finally:
        os.unlink(script_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE, help="hex/bin/elf для загрузки")
    parser.add_argument("--jlink", help="путь к JLink.exe / JLinkExe")
    parser.add_argument("--reset-only", action="store_true", help="не загружать, только сброс и запуск")
    parser.add_argument("--dual-loader-dir", type=Path, default=ROOT / 'tools' / 'jlink', help="каталог официального dual-bank загрузчика SEGGER")
    parser.add_argument("--single-bank", action="store_true", help="только для платы с nDBANK=1; стандартный загрузчик J-Link")
    args = parser.parse_args(argv)

    image: Path | None = None if args.reset_only else args.image
    loader_dir = None if image is None or args.single_bank else args.dual_loader_dir
    if loader_dir is not None and not (loader_dir / 'JLinkDevices.xml').is_file():
        print('Dual-bank loader missing. Run: python tools/setup_dual_loader.py', file=sys.stderr)
        return 2
    if image is not None and not image.exists():
        print(f"образ не найден: {image} — сначала cmake --workflow --preset target", file=sys.stderr)
        return 2
    try:
        jlink = find_jlink(args.jlink)
    except FileNotFoundError as exc:
        print(exc, file=sys.stderr)
        return 2
    code = run(jlink, build_script(image.resolve() if image else None, loader_dir))
    if code != 0:
        print(f"J-Link завершился с кодом {code}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
