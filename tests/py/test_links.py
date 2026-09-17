"""Канал TCP интерфейса: строки уходят с терминатором, ответы режутся по строкам (MI-114)."""

from __future__ import annotations

import socket
import threading

import pytest

from contr_ui.links import TcpLink


class EchoServer:
    """Однократный TCP-сервер: принимает одно соединение, отвечает `echo:<строка>`."""

    def __init__(self) -> None:
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.received: list[bytes] = []
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        conn, _ = self.sock.accept()
        with conn:
            buf = b""
            while True:
                data = conn.recv(1024)
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    self.received.append(line)
                    if line == b"BYE":
                        return
                    # два ответа одним куском: проверяем разрезание по строкам
                    conn.sendall(b"echo:" + line + b"\nG1;OK\n")

    def close(self) -> None:
        self.sock.close()


@pytest.fixture
def server():
    srv = EchoServer()
    yield srv
    srv.close()


def test_send_appends_newline_and_lines_are_split(server: EchoServer) -> None:
    link = TcpLink("127.0.0.1", server.port)
    link.start()
    try:
        link.send("*IDN?")
        assert link.lines.get(timeout=2) == "echo:*IDN?"
        assert link.lines.get(timeout=2) == "G1;OK"
        assert server.received == [b"*IDN?"]
    finally:
        link.close()


def test_remote_close_is_reported_as_none(server: EchoServer) -> None:
    link = TcpLink("127.0.0.1", server.port)
    link.start()
    try:
        link.send("BYE")
        assert link.lines.get(timeout=2) is None
    finally:
        link.close()


def test_connection_refused_raises() -> None:
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    free_port = probe.getsockname()[1]
    probe.close()
    with pytest.raises(OSError):
        TcpLink("127.0.0.1", free_port, timeout=1.0)
