"""Эмулятор устройства CONTR (FW-248…FW-250). ``python -m contr_emu`` — TCP-сервер 5025.

Контракт для платформы (FW-249): класс Emulator без аргументов, ``supports_nonblocking_poll``,
``write(data)`` принимает байты строк с ``\\n``, ``read(timeout_s=0)`` возвращает готовые байты
ответа немедленно либо поднимает TimeoutError.
"""

from __future__ import annotations

from contr_emu.device import Device

__version__ = "0.3.0"


class Emulator:
    """Один TCP-клиент в памяти: платформа пишет команды, читает ответы."""

    supports_nonblocking_poll = True

    def __init__(self) -> None:
        self.device = Device()
        self.device.connect()
        self._partial = b""
        self._out = bytearray()

    def write(self, data: bytes) -> None:
        self._partial += data
        while b"\n" in self._partial:
            line, _, self._partial = self._partial.partition(b"\n")
            reply = self.device.execute("tcp", line.decode("ascii", "replace"))
            if reply is not None:
                self._out += (reply + "\n").encode("ascii")
            if self.device.upd_state == "COMMITTED":
                self.device.reboot()
                self.device.connect()

    def read(self, timeout_s: float = 0) -> bytes:
        if not self._out:
            raise TimeoutError("нет данных")
        data = bytes(self._out)
        self._out.clear()
        return data

    def close(self) -> None:
        self.device.disconnect()


__all__ = ["Device", "Emulator", "__version__"]
