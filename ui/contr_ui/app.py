"""Окно ручного интерфейса: терминал, подключение по TCP и COM, кнопка SAFE (MI-110, MI-112, MI-114).

Стек — tkinter из стандартной библиотеки Python: кроссплатформенно, ничего ставить
не нужно (единственная зависимость репозитория — pyserial для COM-порта).
Шаг 0 — каркас: терминал с историей и подключение; журнал, панели и сценарии
приходят с шага 2 плана 22.
"""

from __future__ import annotations

import argparse
import queue
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import ttk, filedialog, messagebox

from contr_ui import __version__
from contr_ui.links import Link, SerialLink, TcpLink, serial_ports
from contr_ui.workflows import COMMANDS, UpdateImage, parse_script, payload

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
        logdir = Path.cwd() / "logs"
        logdir.mkdir(exist_ok=True)
        self.logfile = (logdir / (datetime.now().strftime("contr-%Y%m%d-%H%M%S-%f") + ".log")).open("a", encoding="utf-8", buffering=1)
        self.pending = None
        self.requests = []
        self.next_poll = 0.0
        self.poll_interval = tk.IntVar(value=500)
        self.completions = list(COMMANDS)
        self.script = []
        self.script_running = False
        self.update_commands = []
        self.updating = False
        self.reconnect_until = 0.0
        self.confirm_deadline = 0.0
        self.no_confirm = tk.BooleanVar(value=False)
        self.last_device_log = None

        self._build_connection_bar(host, port, com, baud)
        self._build_terminal()
        self._build_tools()
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
        self.entry.bind("<Tab>", self._complete)
        ttk.Button(entry_row, text="Отправить", command=self._on_enter).pack(side=tk.LEFT, padx=(4, 0))
        self.entry.focus_set()

    def _build_status(self) -> None:
        self.status = tk.StringVar(value="не подключено")
        ttk.Label(self, textvariable=self.status, anchor=tk.W, padding=(6, 2)).pack(fill=tk.X)

    def _build_tools(self) -> None:
        bar = ttk.Frame(self, padding=6)
        bar.pack(fill=tk.X)
        ttk.Button(bar, text="Сценарий…", command=self._load_script).pack(side=tk.LEFT)
        ttk.Button(bar, text="Стоп сценария", command=self._stop_script).pack(side=tk.LEFT)
        ttk.Button(bar, text="TEST:ALL?", command=lambda: self._send("TEST:ALL?", self._report_test)).pack(side=tk.LEFT)
        ttk.Button(bar, text="Обновить…", command=self._load_update).pack(side=tk.LEFT)
        ttk.Checkbutton(bar, text="Не подтверждать (откат)", variable=self.no_confirm).pack(side=tk.LEFT)
        ttk.Label(bar, text="Опрос, мс:").pack(side=tk.LEFT)
        ttk.Spinbox(bar, from_=100, to=10000, increment=100, textvariable=self.poll_interval, width=6).pack(side=tk.LEFT)
        self.progress = ttk.Progressbar(self, maximum=100)
        self.progress.pack(fill=tk.X, padx=6)
        ttk.Label(self, text=f"Журнал: {self.logfile.name}", anchor=tk.W).pack(fill=tk.X, padx=6)
        self.events = tk.Text(self, height=6, state=tk.DISABLED, wrap=tk.WORD)
        self.events.pack(fill=tk.X, padx=6)

    def _complete(self, _event=None):
        text = self.entry.get()
        token = text.rsplit(" ", 1)[-1].rsplit(",", 1)[-1]
        choices = [s for s in self.completions if s.upper().startswith(token.upper())]
        if len(choices) == 1:
            self._set_entry(text[:-len(token)] + choices[0] if token else choices[0])
        elif choices:
            self._append("Дополнения: " + "  ".join(choices), "sys")
        return "break"

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
        self.next_poll = time.monotonic() + 1
        if not self.updating:
            self._send("INTERLOCK:LIST?", self._learn_signals)

    def _learn_signals(self, reply):
        for record in payload(reply).split(";"):
            fields = record.split(",")
            if len(fields) == 5 and fields[1].isdigit():
                self.completions.append(fields[0])
        self.completions = sorted(set(self.completions))

    def _disconnect(self, reason: str) -> None:
        if self.link is None:
            return
        link, self.link = self.link, None
        link.close()
        self.connect_btn.configure(text="Подключить")
        self.status.set("не подключено")
        self._append(f"канал закрыт: {reason}", "sys")
        self.pending = None
        self.requests.clear()
        self._stop_script()
        if self.updating and not self.reconnect_until:
            self.updating = False
            self._append("Обновление прервано: потеря связи до COMMIT", "err")

    # ---- терминал ----

    def _append(self, line: str, tag: str) -> None:
        self.text.configure(state=tk.NORMAL)
        record = f"{datetime.now().isoformat(timespec='milliseconds')} {line}\n"
        self.text.insert(tk.END, record, tag)
        self.text.see(tk.END)
        self.text.configure(state=tk.DISABLED)
        self.logfile.write(record)
        if tag != "out" and hasattr(self, "events"):
            self.events.configure(state=tk.NORMAL)
            self.events.insert(tk.END, record)
            self.events.see(tk.END)
            self.events.configure(state=tk.DISABLED)

    def _send(self, line: str, callback=None) -> None:
        if self.link is None:
            self._append(f"нет подключения, не отправлено: {line}", "err")
            return
        if line == "SAFE":
            self._stop_script()
            self.update_commands.clear()
            if self.updating:
                self.updating = False
                self.reconnect_until = 0
                self._append("Мастер обновления остановлен кнопкой SAFE", "sys")
            # SAFE preempts a long command. Its response follows ERR:ABORTED.
            self.requests.clear()
            if self.pending:
                old = self.pending
                self.pending = (old[0], None, old[2])
                self.requests.append(("SAFE_RESPONSE", callback))
                try:
                    self.link.send("SAFE")
                    self._append("> SAFE", "out")
                except OSError as exc:
                    self._disconnect(str(exc))
                return
        elif self.pending:
            self.requests.append((line, callback))
            return
        try:
            self.link.send(line)
        except OSError as exc:
            self._disconnect(str(exc))
            return
        self._append(f"> {line}", "out")
        self.pending = (line, callback, time.monotonic() + 30)

    def _on_reply(self, line):
        body = payload(line)
        if body == "ERR:BUSY" and not line.startswith("G") and isinstance(self.link, TcpLink):
            self.reconnect_until = 0
            self.updating = False
            self._disconnect("BUSY: устройство занято другим клиентом")
            return
        pending, self.pending = self.pending, None
        if pending and pending[1]:
            pending[1](line)
        if self.link and not self.pending and self.requests:
            command, callback = self.requests.pop(0)
            if command == "SAFE_RESPONSE":
                self.pending = ("SAFE", callback, time.monotonic() + 30)
            else:
                self._send(command, callback)

    def _load_script(self):
        if self.updating or self.script_running:
            return
        name = filedialog.askopenfilename(title="Сценарий команд", filetypes=[("Сценарии", "*.txt"), ("Все файлы", "*")])
        if not name:
            return
        try:
            self.script = parse_script(Path(name).read_text("utf-8"))
        except (ValueError, OSError) as exc:
            self._append(str(exc), "err")
            return
        self.script_running = True
        self._append(f"Сценарий: {name}", "sys")
        self._script_next()

    def _stop_script(self):
        self.script_running = False
        self.script.clear()

    def _script_next(self, reply=None):
        if not self.script_running:
            return
        if reply is not None and payload(reply).startswith("ERR:"):
            self._append("Сценарий остановлен на первой ошибке", "err")
            self._stop_script()
            return
        if not self.link or not self.script:
            self._append("Сценарий завершён" if self.link else "Сценарий остановлен: нет связи", "sys")
            self._stop_script()
            return
        kind, value = self.script.pop(0)
        if kind == "wait":
            self.after(value, self._script_next)
        elif kind == "prompt":
            if messagebox.askokcancel("Действие у стенда", value):
                self._append(f"Подтверждено оператором: {value}", "sys")
                self._script_next()
            else:
                self._stop_script()
        else:
            # SAFE in scripts is sequential; it should not cancel the script itself.
            running = self.script_running
            rest = list(self.script)
            self._send(value, self._script_next)
            if value == "SAFE":
                self.script_running, self.script = running, rest

    def _report_test(self, reply):
        for item in payload(reply).split(";"):
            self._append("Самодиагностика: " + item, "sys")

    def _load_update(self):
        if not isinstance(self.link, TcpLink) or self.pending or self.updating or self.script_running:
            self._append("Обновление: нужен свободный TCP-канал", "err")
            return
        name = filedialog.askopenfilename(title="Образ прошивки", filetypes=[("Образ CONTR", "*.upd")])
        if not name:
            return
        try:
            image = UpdateImage.read(Path(name).read_bytes())
        except (OSError, ValueError) as exc:
            self._append(str(exc), "err")
            return
        self._append(f"Обновление {image.model}-R{image.revision} {image.version}: {len(image.data)} Б", "sys")
        self._send("SAFE", self._update_next)
        self.update_commands = image.commands()
        self.update_total = len(self.update_commands)
        self.updating = True

    def _update_next(self, reply):
        if not self.updating:
            return
        if payload(reply) != "OK":
            self.updating = False
            self._append("Обновление остановлено: " + reply, "err")
            return
        if not self.update_commands:
            self.reconnect_until = time.monotonic() + 90
            self.confirm_deadline = time.monotonic() + 60
            self._disconnect("COMMIT принят; ожидается перезапуск")
            self.after(1000, self._update_reconnect)
            return
        cmd = self.update_commands.pop(0)
        self.progress["value"] = 100 * (self.update_total - len(self.update_commands)) / self.update_total
        self._send(cmd, self._update_next)

    def _update_reconnect(self):
        if not self.updating or not self.reconnect_until:
            return
        if time.monotonic() >= self.reconnect_until:
            self.updating = False
            self.reconnect_until = 0
            self._append("Не удалось подтвердить результат обновления: таймаут связи", "err")
            return
        if not self.link:
            self._toggle_connection()
        if not self.link:
            self.after(1000, self._update_reconnect)
            return
        self._send("SYST:UPD:STAT?", self._update_status)

    def _update_status(self, reply):
        state = payload(reply).split(",")[0]
        if state == "TRIAL":
            remaining = max(0, int(self.confirm_deadline - time.monotonic()))
            self.status.set(f"Пробный запуск: подтверждение в течение {remaining} с")
            if self.no_confirm.get():
                self.after(1000, self._update_reconnect)
            else:
                self._send("SYST:UPD:CONFIRM", self._update_confirmed)
        elif state == "IDLE":
            self.updating = False
            self.reconnect_until = 0
            self._send("SYST:ERR?", self._update_rollback)
        elif state == "CONFIRMED":
            self._update_confirmed("OK")
        else:
            self.updating = False
            self.reconnect_until = 0
            self._append("Неожиданное состояние обновления: " + reply, "err")

    def _update_rollback(self, reply):
        self._append("Обновление: откат подтверждён устройством" if "rolled back" in reply else
                     "Обновление не подтверждено; состояние IDLE, факт отката требует проверки: " + reply, "sys")

    def _update_confirmed(self, reply):
        self.updating = False
        self.reconnect_until = 0
        self._append("Обновление подтверждено" if payload(reply) == "OK" else "Подтверждение не принято: " + reply, "sys")
        self._send("*IDN?")

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
                    self._on_reply(line)
            except queue.Empty:
                pass
        now = time.monotonic()
        if self.pending and now > self.pending[2]:
            self._disconnect("таймаут ответа; канал закрыт для восстановления синхронизации")
        if self.link and not self.pending and not self.requests and not self.script_running and not self.updating and now >= self.next_poll:
            try:
                interval = max(100, self.poll_interval.get())
            except (ValueError, tk.TclError):
                interval = 500
            self.next_poll = now + interval / 1000
            self._send("SYST:ERR?")
            self._send("SYST:LOG?")
        self.after(POLL_MS, self._poll)

    def _on_close(self) -> None:
        self._disconnect("выход")
        self.logfile.close()
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
