"""Окно ручного интерфейса: терминал, подключение по TCP и COM, кнопка SAFE (MI-110, MI-112, MI-114).

Стек — tkinter из стандартной библиотеки Python: кроссплатформенно, ничего ставить
не нужно (единственная зависимость репозитория — pyserial для COM-порта).
Шаг 0 — каркас: терминал с историей и подключение; журнал, панели и сценарии
приходят с шага 2 плана 22.
"""

from __future__ import annotations

import argparse
import queue
import tkinter as tk
from datetime import datetime
from tkinter import ttk

from contr_ui import __version__
from contr_ui.links import Link, SerialLink, TcpLink, serial_ports

POLL_MS = 50
HISTORY_MAX = 200


def stamp() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


class App(tk.Tk):
    def __init__(self, host: str, port: int, com: str | None, baud: int) -> None:
        super().__init__()
        self.title(f"CONTR — ручной интерфейс {__version__}")
        self.geometry("900x600")
        self.link: Link | None = None
        self.history: list[str] = []
        self.history_pos = 0

        self._build_connection_bar(host, port, com, baud)
        self._build_terminal()
        self._build_status()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(POLL_MS, self._poll)

    # ---- построение окна ----

    def _build_connection_bar(self, host: str, port: int, com: str | None, baud: int) -> None:
        bar = ttk.Frame(self, padding=6)
        bar.pack(fill=tk.X)

        self.mode = tk.StringVar(value="tcp")
        ttk.Radiobutton(bar, text="TCP", variable=self.mode, value="tcp").pack(side=tk.LEFT)
        self.host = tk.StringVar(value=host)
        ttk.Entry(bar, textvariable=self.host, width=16).pack(side=tk.LEFT, padx=(2, 0))
        self.port = tk.StringVar(value=str(port))
        ttk.Entry(bar, textvariable=self.port, width=6).pack(side=tk.LEFT, padx=(2, 12))

        ttk.Radiobutton(bar, text="COM", variable=self.mode, value="com").pack(side=tk.LEFT)
        self.com = tk.StringVar(value=com or "")
        self.com_box = ttk.Combobox(bar, textvariable=self.com, width=14, postcommand=self._refresh_ports)
        self.com_box.pack(side=tk.LEFT, padx=(2, 0))
        self.baud = tk.StringVar(value=str(baud))
        ttk.Entry(bar, textvariable=self.baud, width=8).pack(side=tk.LEFT, padx=(2, 12))

        self.connect_btn = ttk.Button(bar, text="Подключить", command=self._toggle_connection)
        self.connect_btn.pack(side=tk.LEFT)

        # MI-112: одна кнопка SAFE, отдельно от остального, без подтверждения
        self.safe_btn = tk.Button(bar, text="SAFE", bg="#c62828", fg="white", font=("TkDefaultFont", 11, "bold"),
                                  width=8, command=lambda: self._send("SAFE"))
        self.safe_btn.pack(side=tk.RIGHT)

    def _build_terminal(self) -> None:
        frame = ttk.Frame(self, padding=(6, 0, 6, 6))
        frame.pack(fill=tk.BOTH, expand=True)
        self.text = tk.Text(frame, wrap=tk.NONE, state=tk.DISABLED, font=("Consolas", 10))
        scroll = ttk.Scrollbar(frame, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.tag_configure("out", foreground="#1565c0")
        self.text.tag_configure("in", foreground="#2e7d32")
        self.text.tag_configure("err", foreground="#c62828")
        self.text.tag_configure("sys", foreground="#757575")
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        entry_row = ttk.Frame(self, padding=(6, 0, 6, 6))
        entry_row.pack(fill=tk.X)
        self.entry = ttk.Entry(entry_row, font=("Consolas", 10))
        self.entry.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.entry.bind("<Return>", self._on_enter)
        self.entry.bind("<Up>", self._history_up)
        self.entry.bind("<Down>", self._history_down)
        ttk.Button(entry_row, text="Отправить", command=self._on_enter).pack(side=tk.LEFT, padx=(4, 0))
        self.entry.focus_set()

    def _build_status(self) -> None:
        self.status = tk.StringVar(value="не подключено")
        ttk.Label(self, textvariable=self.status, anchor=tk.W, padding=(6, 2)).pack(fill=tk.X)

    # ---- подключение ----

    def _refresh_ports(self) -> None:
        self.com_box["values"] = [dev for dev, _ in serial_ports()]

    def _toggle_connection(self) -> None:
        if self.link is not None:
            self._disconnect("отключено пользователем")
            return
        try:
            if self.mode.get() == "tcp":
                link: Link = TcpLink(self.host.get().strip(), int(self.port.get()))
            else:
                if not self.com.get().strip():
                    raise ValueError("выберите COM-порт")
                link = SerialLink(self.com.get().strip(), int(self.baud.get()))
        except Exception as exc:
            self._append(f"не удалось подключиться: {exc}", "err")
            return
        self.link = link
        link.start()
        self.connect_btn.configure(text="Отключить")
        self.status.set(f"подключено: {link.label}")
        self._append(f"подключено: {link.label}", "sys")

    def _disconnect(self, reason: str) -> None:
        if self.link is None:
            return
        link, self.link = self.link, None
        link.close()
        self.connect_btn.configure(text="Подключить")
        self.status.set("не подключено")
        self._append(f"канал закрыт: {reason}", "sys")

    # ---- терминал ----

    def _append(self, line: str, tag: str) -> None:
        self.text.configure(state=tk.NORMAL)
        self.text.insert(tk.END, f"{stamp()} {line}\n", tag)
        self.text.see(tk.END)
        self.text.configure(state=tk.DISABLED)

    def _send(self, line: str) -> None:
        if self.link is None:
            self._append(f"нет подключения, не отправлено: {line}", "err")
            return
        try:
            self.link.send(line)
        except OSError as exc:
            self._disconnect(str(exc))
            return
        self._append(f"> {line}", "out")

    def _on_enter(self, _event: object = None) -> None:
        line = self.entry.get().strip()
        if not line:
            return
        self.entry.delete(0, tk.END)
        if not self.history or self.history[-1] != line:
            self.history.append(line)
            del self.history[:-HISTORY_MAX]
        self.history_pos = len(self.history)
        self._send(line)

    def _history_up(self, _event: object) -> str:
        if self.history and self.history_pos > 0:
            self.history_pos -= 1
            self._set_entry(self.history[self.history_pos])
        return "break"

    def _history_down(self, _event: object) -> str:
        if self.history_pos < len(self.history) - 1:
            self.history_pos += 1
            self._set_entry(self.history[self.history_pos])
        else:
            self.history_pos = len(self.history)
            self._set_entry("")
        return "break"

    def _set_entry(self, text: str) -> None:
        self.entry.delete(0, tk.END)
        self.entry.insert(0, text)

    def _poll(self) -> None:
        link = self.link
        if link is not None:
            try:
                while True:
                    line = link.lines.get_nowait()
                    if line is None:
                        self._disconnect("устройство закрыло соединение")
                        break
                    tag = "err" if line.split(";", 1)[-1].startswith("ERR:") else "in"
                    self._append(f"< {line}", tag)
            except queue.Empty:
                pass
        self.after(POLL_MS, self._poll)

    def _on_close(self) -> None:
        self._disconnect("выход")
        self.destroy()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="contr_ui", description="Ручной интерфейс стенда CONTR")
    parser.add_argument("--host", default="192.168.0.20", help="адрес устройства или эмулятора")
    parser.add_argument("--port", type=int, default=5025)
    parser.add_argument("--com", help="COM-порт консоли, включает режим COM")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args(argv)
    app = App(args.host, args.port, args.com, args.baud)
    if args.com:
        app.mode.set("com")
    app.mainloop()
    return 0
