"""TCP-сервер эмулятора: ``python -m contr_emu [--port 5025] [--console]``.

Один клиент; второе соединение получает ``ERR:BUSY`` и закрывается (FW-102). Обрыв
соединения — вход в безопасное состояние (FW-218). После ``SYST:UPD:COMMIT`` эмулятор
«перезагружается»: рвёт соединение, применяет сетевую запись и стартует пробный запуск.
С ключом ``--console`` строки со stdin исполняются как консоль USART3.
"""

from __future__ import annotations

import argparse
import selectors
import socket
import sys
import threading
import time

from contr_emu.device import Device


def serve(port: int, console: bool, once: bool = False) -> None:
    dev = Device()
    lock = threading.Lock()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(2)
    listener.setblocking(False)
    print(f"contr_emu: listening 127.0.0.1:{port}", file=sys.stderr, flush=True)

    if console:
        def console_loop() -> None:
            for line in sys.stdin:
                with lock:
                    reply = dev.execute("console", line)
                if reply is not None:
                    print(reply, flush=True)
        threading.Thread(target=console_loop, daemon=True).start()

    sel = selectors.DefaultSelector()
    sel.register(listener, selectors.EVENT_READ)
    client: socket.socket | None = None
    buf = b""
    last_tick = time.monotonic()
    while True:
        now = time.monotonic()
        elapsed = int((now - last_tick) * 1000)
        if elapsed:
            last_tick += elapsed / 1000
            with lock:
                dev.advance(elapsed)
                disconnected = client is not None and not dev.client_connected
            if disconnected:
                sel.unregister(client)
                client.close()
                client = None
                buf = b""
        for key, _ in sel.select(timeout=0.05):
            if key.fileobj is listener:
                conn, _ = listener.accept()
                with lock:
                    accepted = dev.connect()
                if not accepted:
                    try:
                        conn.sendall(b"ERR:BUSY\n")
                    finally:
                        conn.close()
                    continue
                conn.setblocking(False)
                client = conn
                buf = b""
                sel.register(conn, selectors.EVENT_READ)
            else:
                conn = key.fileobj
                try:
                    data = conn.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    sel.unregister(conn)
                    conn.close()
                    client = None
                    with lock:
                        dev.disconnect()
                    if once:
                        return
                    continue
                buf += data
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    with lock:
                        reply = dev.execute("tcp", line.decode("ascii", "replace"))
                        rebooting = dev.upd_state == "COMMITTED"
                    if reply is not None:
                        try:
                            conn.sendall((reply + "\n").encode("ascii"))
                        except OSError:
                            pass
                    if rebooting:
                        sel.unregister(conn)
                        conn.close()
                        client = None
                        with lock:
                            dev.reboot()
                        break
        _ = client


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="contr_emu", description=__doc__)
    parser.add_argument("--port", type=int, default=5025)
    parser.add_argument("--console", action="store_true", help="stdin как консоль USART3")
    args = parser.parse_args(argv)
    try:
        serve(args.port, args.console)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
