"""Каналы связи интерфейса: TCP к плате или эмулятору и COM-порт консоли (MI-114).

Оба канала строчные: send() дописывает `\\n`, принятые байты режутся по `\\n`
и складываются в очередь строк. Чтение идёт в фоновом потоке, окно забирает
строки из очереди по таймеру — GUI-библиотека потоков не видит.
"""

from __future__ import annotations

import queue
import socket
import threading
from abc import ABC, abstractmethod


class LinkClosed(Exception):
    """Канал закрыт: сокет разорван или порт исчез."""


class Link(ABC):
    """Строчный канал к устройству."""

    def __init__(self) -> None:
        self.lines: queue.Queue[str | None] = queue.Queue()
        self._buf = bytearray()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    @abstractmethod
    def label(self) -> str: ...

    @abstractmethod
    def _read(self) -> bytes:
        """Вернуть принятые байты (пусто — таймаут); LinkClosed при разрыве."""

    @abstractmethod
    def _write(self, data: bytes) -> None: ...

    @abstractmethod
    def _close(self) -> None: ...

    def start(self) -> None:
        self._thread = threading.Thread(target=self._pump, name=f"link:{self.label}", daemon=True)
        self._thread.start()

    def send(self, line: str) -> None:
        self._write(line.encode("ascii", "replace") + b"\n")

    def close(self) -> None:
        self._stop.set()
        self._close()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=1.0)

    def _pump(self) -> None:
        try:
            while not self._stop.is_set():
                chunk = self._read()
                if not chunk:
                    continue
                self._buf.extend(chunk)
                while b"\n" in self._buf:
                    line, _, rest = self._buf.partition(b"\n")
                    self._buf[:] = rest
                    self.lines.put(line.decode("ascii", "replace").rstrip("\r"))
        except LinkClosed:
            pass
        except OSError:
            pass
        finally:
            self.lines.put(None)  # признак закрытия для окна


class TcpLink(Link):
    """TCP к устройству (порт 5025, FW-100) или к эмулятору (FW-248)."""

    def __init__(self, host: str, port: int = 5025, timeout: float = 3.0) -> None:
        super().__init__()
        self._host, self._port = host, port
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(0.1)

    @property
    def label(self) -> str:
        return f"tcp {self._host}:{self._port}"

    def _read(self) -> bytes:
        try:
            data = self._sock.recv(4096)
        except TimeoutError:
            return b""
        if not data:
            raise LinkClosed
        return data

    def _write(self, data: bytes) -> None:
        self._sock.sendall(data)

    def _close(self) -> None:
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()


class SerialLink(Link):
    """Консоль USART3 через COM-порт отладчика (FW-114)."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        super().__init__()
        import serial  # только здесь: тесты TCP не требуют pyserial

        self._port_name = port
        self._ser = serial.Serial(port, baud, timeout=0.1, write_timeout=0.5)

    @property
    def label(self) -> str:
        return f"com {self._port_name}"

    def _read(self) -> bytes:
        try:
            return self._ser.read(4096)
        except Exception as exc:  # SerialException при исчезновении порта
            raise LinkClosed from exc

    def _write(self, data: bytes) -> None:
        if self._ser.write(data) != len(data):
            raise OSError("Incomplete serial write")

    def _close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


def serial_ports() -> list[tuple[str, str]]:
    """Доступные COM-порты: (устройство, описание). Отладчики — первыми."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    known = {0x1366: "J-Link", 0x0483: "ST-LINK"}
    ports = [(p.device, f"{known.get(p.vid or -1, '')} {p.description}".strip()) for p in list_ports.comports()]
    ports.sort(key=lambda item: (not item[1].startswith(("J-Link", "ST-LINK")), item[0]))
    return ports
