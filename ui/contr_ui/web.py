"""Local HTTP bridge and Chrome interface. No web service or account required."""
from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import shutil
import subprocess
import sys
import threading
from concurrent.futures import TimeoutError as FutureTimeout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from .bridge import Bridge, DeviceError
from .links import serial_ports

ASSETS = Path(__file__).with_name("static")


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, bridge):
        self.bridge = bridge
        self.token = secrets.token_urlsafe(32)
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    server: Server

    def log_message(self, *_):
        pass

    def _local(self):
        allowed = {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}
        return self.headers.get("Host") in allowed and self.headers.get("Sec-Fetch-Site") != "cross-site"

    def _reply(self, status, data, content_type="application/json; charset=utf-8", attachment=False):
        if isinstance(data, (dict, list)):
            data = json.dumps(data, ensure_ascii=False).encode("utf-8")
        elif isinstance(data, str):
            data = data.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        if attachment:
            self.send_header("Content-Disposition", f'attachment; filename="{self.server.bridge.log_path.name}"')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if not self._local():
            return self._reply(403, {"error": "Доступ только с локальной страницы CONTR"})
        url = urlsplit(self.path)
        path = url.path
        bridge = self.server.bridge
        if path == "/api/bootstrap":
            return self._reply(200, {"token": self.server.token, "ports": serial_ports(), "version": "0.2.0"})
        if path == "/api/state":
            try:
                since = max(0, int(parse_qs(url.query).get("since", ["0"])[0]))
            except ValueError:
                return self._reply(400, {"error": "Неверный курсор"})
            return self._reply(200, bridge.snapshot(since))
        if path == "/api/log":
            return self._reply(200, bridge.log_path.read_bytes(), "application/x-ndjson; charset=utf-8", True)
        if path == "/api/ports":
            return self._reply(200, serial_ports())
        assets = {"/": ("index.html", "text/html"), "/terminal": ("index.html", "text/html"),
                  "/app.js": ("app.js", "text/javascript"), "/style.css": ("style.css", "text/css")}
        if path not in assets:
            return self._reply(404, {"error": "Страница не найдена"})
        name, mime = assets[path]
        self._reply(200, (ASSETS / name).read_bytes(), mime + "; charset=utf-8")

    def do_POST(self):
        origin = self.headers.get("Origin")
        if (not self._local() or self.headers.get("X-Contr-Token") != self.server.token
                or origin and origin != "http://" + self.headers.get("Host", "")):
            return self._reply(403, {"error": "Запрос отклонён: откройте локальную страницу CONTR"})
        if self.headers.get("Content-Type", "").split(";", 1)[0] != "application/json":
            return self._reply(415, {"error": "Требуется application/json"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 <= length <= 1500000:
                return self._reply(413, {"error": "Слишком большой запрос"})
            data = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(data, dict):
                raise ValueError("Ожидается объект JSON")
            bridge = self.server.bridge
            path = urlsplit(self.path).path
            if path == "/api/connect":
                result = bridge.submit("connect", target=data).result(timeout=40)
            elif path == "/api/disconnect":
                result = bridge.submit("disconnect").result(timeout=35)
            elif path == "/api/command":
                cmd = str(data["command"]).strip()
                if cmd.upper() == "SAFE":
                    bridge.safe()
                    result = {"accepted": True}
                else:
                    result = bridge.submit("command", command=cmd).result(timeout=35)
            elif path == "/api/safe":
                bridge.safe()
                result = {"accepted": True}
            elif path == "/api/refresh":
                result = bridge.submit("refresh").result(timeout=40)
            elif path == "/api/poll":
                bridge.set_poll(int(data["ms"]))
                result = {"ok": True}
            elif path == "/api/script":
                bridge.start_job("script", text=str(data["text"]), name=str(data.get("name", "Сценарий")))
                result = {"accepted": True}
            elif path == "/api/update":
                binary = base64.b64decode(data["base64"], validate=True)
                bridge.start_job("update", data=binary, confirm=data.get("confirm", True))
                result = {"accepted": True}
            elif path == "/api/job/stop":
                bridge.cancel_job()
                result = {"ok": True}
            elif path == "/api/job/answer":
                bridge.answer_prompt(bool(data["accepted"]))
                result = {"ok": True}
            elif path == "/api/update/confirm":
                bridge.set_confirm(bool(data["confirm"]))
                result = {"ok": True}
            else:
                return self._reply(404, {"error": "Неизвестное действие"})
            self._reply(200, result or {"ok": True})
        except (ValueError, KeyError, TypeError) as exc:
            self._reply(400, {"error": str(exc)})
        except (DeviceError, OSError, FutureTimeout) as exc:
            self._reply(409, {"error": str(exc) or "Операция не завершилась вовремя"})


def chrome(url):
    candidates = [shutil.which("google-chrome"), shutil.which("chrome"),
                  os.path.join(os.environ.get("PROGRAMFILES", "C:/Program Files"), "Google/Chrome/Application/chrome.exe"),
                  os.path.join(os.environ.get("PROGRAMFILES(X86)", "C:/Program Files (x86)"), "Google/Chrome/Application/chrome.exe"),
                  os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google/Chrome/Application/chrome.exe"),
                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
    executable = next((c for c in candidates if c and Path(c).is_file()), None)
    if not executable:
        print(f"Google Chrome не найден; откройте в Chrome: {url}", flush=True)
        return
    subprocess.Popen([executable, "--new-window", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main(argv=None):
    parser = argparse.ArgumentParser(description="CONTR — панели управления в Google Chrome")
    parser.add_argument("--port", type=int, default=8765, help="локальный HTTP-порт")
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--log-dir", type=Path, default=Path("logs"))
    args = parser.parse_args(argv)
    bridge = Bridge(args.log_dir)
    server = Server(("127.0.0.1", args.port), bridge)
    url = f"http://127.0.0.1:{server.server_port}"
    print(f"CONTR: {url}; журнал {bridge.log_path}", flush=True)
    if not args.no_browser:
        chrome(url)
    try:
        server.serve_forever(poll_interval=.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        bridge.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
