"""Single-owner device session for the local browser interface.

Only the worker consumes device replies. SAFE can interrupt a pending operation,
but its reply is drained before another command is issued. HTTP threads never read
the serial port or TCP stream directly.
"""
from __future__ import annotations

import copy
import json
import queue
import threading
import time
from collections import deque
from concurrent.futures import Future
from datetime import datetime
from pathlib import Path

from .links import Link, SerialLink, TcpLink
from .workflows import COMMANDS, UpdateImage, parse_script, payload


class DeviceError(RuntimeError):
    pass


class Cancelled(DeviceError):
    pass


class Bridge:
    def __init__(self, log_dir: Path):
        log_dir.mkdir(parents=True, exist_ok=True)
        self.log_path = log_dir / f"contr-{datetime.now():%Y%m%d-%H%M%S-%f}.jsonl"
        self.log = self.log_path.open("a", encoding="utf-8", buffering=1)
        self.lock = threading.RLock()
        self.tasks: queue.Queue = queue.Queue()
        self.epoch = 0
        self.active_epoch = None
        self.stop = threading.Event()
        self.cancel = threading.Event()
        self.urgent = threading.Event()
        self.answer = threading.Event()
        self.link: Link | None = None
        self.target = None
        self.sequence = 0
        self.events = deque(maxlen=2000)
        self.previous_log = []
        self.poll_ms = 500
        self.next_poll = 0.0
        self.poll_index = 0
        self.confirm_update = True
        self.state = {"connected": False, "channel": "", "target": None, "generation": None,
                      "replies": {}, "commands": list(COMMANDS), "relays": [], "job": None,
                      "poll_ms": 500, "last_error": "", "log_name": self.log_path.name,
                      "safe_acknowledged": False}
        self.worker = threading.Thread(target=self._loop, name="contr-session", daemon=True)
        self.worker.start()

    def event(self, kind, text, **extra):
        with self.lock:
            self.sequence += 1
            row = {"id": self.sequence, "time": datetime.now().isoformat(timespec="milliseconds"),
                   "kind": kind, "text": text, **extra}
            self.events.append(row)
            self.log.write(json.dumps(row, ensure_ascii=False) + "\n")

    def snapshot(self, since=0):
        with self.lock:
            result = copy.deepcopy(self.state)
            result["events"] = [e for e in self.events if e["id"] > since]
            result["cursor"] = self.sequence
            result["events_truncated"] = bool(self.events and since and since < self.events[0]["id"] - 1)
            job = result["job"]
            if job and job.get("deadline"):
                job["remaining_s"] = max(0, round(job["deadline"] - time.monotonic()))
                del job["deadline"]
            return result

    def _job_active(self):
        return self.state["job"] and self.state["job"]["status"] in {"running", "waiting", "reconnecting", "trial"}

    def submit(self, operation, **args):
        future = Future()
        with self.lock:
            if self._job_active():
                raise DeviceError("Сначала остановите выполняемый сценарий или обновление")
            if self.urgent.is_set():
                raise DeviceError("Дождитесь ответа SAFE")
            self.tasks.put((operation, args, future, self.epoch))
        return future

    def safe(self):
        with self.lock:
            if not self.state["connected"]:
                raise DeviceError("Нет подключения к устройству")
            self.epoch += 1
            self.cancel.set()
            self.urgent.set()
            self.answer.set()
        self.event("system", "Запрошен SAFE; текущая операция будет остановлена")

    def cancel_job(self):
        self.cancel.set()
        self.answer.set()
        self.event("system", "Остановить операцию")

    def answer_prompt(self, accepted):
        with self.lock:
            if not self.state["job"] or self.state["job"]["status"] != "waiting":
                raise DeviceError("Нет ожидающего подтверждения шага")
        if not accepted:
            self.cancel.set()
        self.event("system", "Действие оператора подтверждено" if accepted else "Действие оператора отменено")
        self.answer.set()

    def set_confirm(self, value):
        self.confirm_update = bool(value)
        self.event("system", "Подтверждение пробного запуска включено" if value else "Пробный запуск не подтверждать: ожидается откат")

    def set_poll(self, value):
        if not 100 <= value <= 10000:
            raise ValueError("Период опроса: 100…10000 мс")
        self.poll_ms = value
        with self.lock:
            self.state["poll_ms"] = value

    def start_job(self, kind, **args):
        with self.lock:
            if self._job_active() or not self.tasks.empty() or self.urgent.is_set():
                raise DeviceError("Дождитесь завершения текущей операции")
            if not self.state["connected"]:
                raise DeviceError("Подключите устройство")
            if kind == "update" and self.target["mode"] == "serial":
                raise DeviceError("Обновление выполняется по Ethernet")
            # Validate before reserving the session or touching the device.
            if kind == "script":
                args["steps"] = parse_script(args.pop("text"))
            elif kind == "update":
                args["image"] = UpdateImage.read(args.pop("data"))
                self.confirm_update = bool(args.pop("confirm", True))
            else:
                raise ValueError("Неизвестная операция")
            self.cancel.clear()
            self.answer.clear()
            self.state["job"] = {"kind": kind, "status": "running", "progress": 0, "message": "Подготовка"}
            self.tasks.put((kind, args, Future(), self.epoch))

    def _job(self, **values):
        with self.lock:
            self.state["job"].update(values)

    def _open(self, target):
        mode = target.get("mode", "tcp")
        if mode not in {"tcp", "emulator", "serial"}:
            raise ValueError("Неизвестный канал")
        if mode == "serial":
            port = str(target.get("com", "")).strip()
            if not port:
                raise ValueError("Укажите COM-порт")
            link = SerialLink(port, int(target.get("baud", 115200)))
        else:
            host = str(target.get("host", "127.0.0.1" if mode == "emulator" else "192.168.0.20")).strip()
            port = int(target.get("port", 5025))
            if not host or not 1 <= port <= 65535:
                raise ValueError("Неверный адрес или порт")
            link = TcpLink(host, port, timeout=2)
        link.start()
        self.link = link
        self.target = dict(target)
        self.previous_log = []
        with self.lock:
            self.state.update(connected=True, channel=link.label, target=self.target, last_error="",
                              generation=None, replies={}, relays=[], commands=list(COMMANDS), safe_acknowledged=False)
        self.event("system", "Подключено: " + link.label)

    def _close(self, reason):
        if self.link:
            self.link.close()
            self.link = None
            self.event("system", "Канал закрыт: " + reason)
        with self.lock:
            self.state["connected"] = False
            self.state["safe_acknowledged"] = False

    def _observe(self, command, reply):
        body = payload(reply)
        with self.lock:
            if reply.startswith("G") and ";" in reply:
                try:
                    self.state["generation"] = int(reply[1:reply.index(";")])
                except ValueError:
                    pass
            if body.startswith("ERR:"):
                self.state["last_error"] = body
                return
            if command == "SAFE":
                self.state["safe_acknowledged"] = body == "OK"
                if body == "OK":
                    self.state["replies"]["ROUT:STAT?"] = ""
            elif not command.split(" ", 1)[0].endswith("?"):
                self.state["safe_acknowledged"] = False
            if command.endswith("?"):
                self.state["replies"][command] = body
            if command == "INTERLOCK:LIST?":
                rows = []
                for item in body.split(";"):
                    cells = item.split(",")
                    if len(cells) == 5 and cells[1].isdigit():
                        rows.append(dict(zip(("name", "number", "block", "address", "groups"), cells)))
                self.state["relays"] = rows
                self.state["commands"] = list(COMMANDS) + [r["name"] for r in rows]

    def _read_reply(self, deadline, current, background=False):
        while time.monotonic() < deadline and not self.stop.is_set():
            try:
                reply = self.link.lines.get(timeout=.05)
            except queue.Empty:
                continue
            if reply is None:
                raise ConnectionError("Устройство закрыло соединение")
            if not reply:
                continue
            if reply == "ERR:BUSY" and isinstance(self.link, TcpLink):
                self.event("error", "BUSY: устройство занято другим клиентом")
                raise DeviceError("BUSY: устройство занято другим клиентом; повторное подключение отключено")
            self._observe(current, reply)
            return reply
        raise TimeoutError("Таймаут ответа; переподключитесь для восстановления синхронизации")

    def _exchange(self, command, background=False):
        if not self.link:
            raise DeviceError("Нет подключения")
        command = command.strip()
        if not command or not command.isascii() or any(c in command for c in "\r\n\0") or len(command) > 480:
            raise ValueError("Одна ASCII-команда, до 480 символов")
        started = time.monotonic()
        deadline = started + 30
        safe_sent = False
        try:
            with self.lock:
                if command != "SAFE" and self.active_epoch is not None and self.active_epoch != self.epoch:
                    raise Cancelled("Команда отменена запросом SAFE")
                self.event("tx", command, background=background)
                self.link.send(command)
            while True:
                if self.urgent.is_set() and command != "SAFE" and not safe_sent:
                    self.urgent.clear()
                    self.link.send("SAFE")
                    self.event("tx", "SAFE", background=False)
                    safe_sent = True
                if not self.link.lines.empty():
                    reply = self._read_reply(deadline, command, background)
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Таймаут ответа")
                if self.stop.wait(.01):
                    raise Cancelled("Сервер остановлен")
            self.event("rx", reply, command=command, background=background, elapsed_ms=round((time.monotonic() - started) * 1000))
            if safe_sent:
                safe_reply = self._read_reply(time.monotonic() + 5, "SAFE")
                self.event("rx", safe_reply, command="SAFE", background=False)
            if command == "SYST:LOG?" and not payload(reply).startswith("ERR:"):
                entries = payload(reply).split(";") if payload(reply) != "NONE" else []
                # Device returns a rolling snapshot: emit only its new suffix.
                overlap = min(len(entries), len(self.previous_log))
                while overlap and self.previous_log[-overlap:] != entries[:overlap]:
                    overlap -= 1
                for entry in entries[overlap:]:
                    self.event("device", entry)
                self.previous_log = entries
            if command == "SYST:ERR?" and payload(reply) != "NONE":
                self.event("error", payload(reply))
            return reply
        except Cancelled:
            raise
        except (OSError, DeviceError) as exc:
            self._close(str(exc))
            raise

    def _require_ok(self, command):
        if self.cancel.is_set():
            raise Cancelled("Операция остановлена")
        reply = self._exchange(command)
        if payload(reply) != "OK":
            raise DeviceError(reply)
        return reply

    def _refresh(self):
        for cmd in ("*IDN?", "SYST:NET?", "SYST:SAFE?", "TEST:ALL?", "SYST:CONF?", "INTERLOCK:LIST?", "SYST:UPD:STAT?", "ROUT:STAT?"):
            if self.urgent.is_set():
                self.urgent.clear()
                self._exchange("SAFE")
            self._exchange(cmd, background=True)
        self.next_poll = time.monotonic() + self.poll_ms / 1000

    def _run_script(self, steps, name="Сценарий"):
        self.event("system", "Сценарий: " + name)
        for i, (kind, value) in enumerate(steps):
            if self.cancel.is_set():
                raise Cancelled("Сценарий остановлен")
            self._job(progress=round(100 * i / max(1, len(steps))), message=str(value), status="running")
            if kind == "wait":
                if self.cancel.wait(value / 1000):
                    raise Cancelled("Сценарий остановлен во время паузы")
            elif kind == "prompt":
                self.answer.clear()
                self._job(status="waiting", message=str(value))
                self.event("prompt", str(value))
                while not self.answer.wait(.1):
                    if self.cancel.is_set() or self.stop.is_set():
                        raise Cancelled("Сценарий остановлен")
                if self.cancel.is_set():
                    raise Cancelled("Сценарий остановлен оператором")
            else:
                reply = self._exchange(str(value))
                if payload(reply).startswith("ERR:"):
                    raise DeviceError("Сценарий остановлен на первой ошибке: " + reply)
        self._job(status="done", progress=100, message="Сценарий завершён")

    def _run_update(self, image: UpdateImage):
        target = dict(self.target)
        self.event("system", f"Обновление {image.model}-R{image.revision} {image.version}; {len(image.data)} байт")
        self._require_ok("SAFE")
        commands = image.commands()
        committed = False
        try:
            for i, command in enumerate(commands):
                self._job(message="Запись образа", progress=round(80 * i / len(commands)))
                if command == "SYST:UPD:COMMIT":
                    committed = True
                    try:
                        self._require_ok(command)
                    except (ConnectionError, TimeoutError):
                        self.event("system", "COMMIT: связь закрыта; проверяем состояние после перезапуска")
                else:
                    self._require_ok(command)
        except Exception:
            if self.link and not committed:
                self._exchange("SYST:UPD:ABORT")
            raise
        self._close("COMMIT; ожидание перезапуска")
        reconnect_deadline = time.monotonic() + 90
        trial_deadline = time.monotonic() + 60
        self._job(status="reconnecting", message="Ожидание устройства", progress=85)
        saw_trial = False
        while time.monotonic() < reconnect_deadline:
            if self.cancel.is_set():
                raise Cancelled("Наблюдение остановлено; неподтверждённый образ должен откатиться сам")
            if self.cancel.wait(.4):
                continue
            try:
                if not self.link:
                    self._open(target)
                state = payload(self._exchange("SYST:UPD:STAT?")).split(",")[0]
                if state == "TRIAL":
                    saw_trial = True
                    self._job(status="trial", progress=90, message="Пробный запуск", deadline=trial_deadline)
                    if self.confirm_update:
                        identity = payload(self._exchange("*IDN?")).split(",")
                        if len(identity) != 4 or identity[1] != f"{image.model}-R{image.revision}" or identity[3] != image.version:
                            raise DeviceError("Пробный образ не совпадает с выбранной моделью/версией; CONFIRM не отправлен")
                        self._require_ok("SYST:UPD:CONFIRM")
                        self._exchange("SYST:UPD:STAT?")
                        self._exchange("*IDN?")
                        self._job(status="done", progress=100, message="Обновление подтверждено", deadline=None)
                        self.event("system", "Обновление подтверждено устройством")
                        return
                elif state == "CONFIRMED":
                    identity = payload(self._exchange("*IDN?")).split(",")
                    if len(identity) != 4 or identity[1] != f"{image.model}-R{image.revision}" or identity[3] != image.version:
                        raise DeviceError("Подтверждённый образ не совпадает с выбранной моделью/версией")
                    self._job(status="done", progress=100, message="Обновление подтверждено", deadline=None)
                    return
                elif state == "IDLE":
                    errors = self._exchange("SYST:ERR?")
                    self._exchange("*IDN?")
                    if "rolled back" in errors:
                        self._job(status="rolled_back", progress=100, message="Откат подтверждён устройством", deadline=None)
                        self.event("system", "Откат подтверждён устройством")
                        return
                    raise DeviceError("Устройство в IDLE; подтверждения обновления или отката нет" + (" после TRIAL" if saw_trial else ""))
                else:
                    raise DeviceError("Неожиданное состояние обновления: " + state)
            except (OSError, ConnectionError):
                self._close("Переподключение после перезапуска")
        raise DeviceError("Не удалось определить результат обновления за 90 с")

    def _dispatch(self, operation, args):
        if operation == "connect":
            self._close("Смена канала")
            self._open(args["target"])
            self._refresh()
            return {"connected": True}
        if operation == "disconnect":
            self._close("Отключено пользователем")
            return {"connected": False}
        if operation == "command":
            return {"reply": self._exchange(args["command"])}
        if operation == "refresh":
            self._refresh()
            return {"refreshed": True}
        if operation == "script":
            return self._run_script(**args)
        if operation == "update":
            return self._run_update(**args)
        raise ValueError("Неизвестная операция")

    def _loop(self):
        while not self.stop.is_set():
            if self.urgent.is_set():
                self.urgent.clear()
                try:
                    self._exchange("SAFE")
                except Exception as exc:
                    self.event("error", str(exc))
            try:
                operation, args, future, epoch = self.tasks.get(timeout=.05)
            except queue.Empty:
                if self.link and time.monotonic() >= self.next_poll:
                    commands = ("ROUT:STAT?", "SYST:ERR?", "ROUT:STAT?", "SYST:LOG?", "ROUT:STAT?", "SYST:SAFE?", "ROUT:STAT?", "SYST:NET?")
                    cmd = commands[self.poll_index % len(commands)]
                    self.poll_index += 1
                    self.next_poll = time.monotonic() + self.poll_ms / 1000
                    try:
                        self._exchange(cmd, background=True)
                    except Exception as exc:
                        self.event("error", str(exc))
                continue
            try:
                with self.lock:
                    if epoch != self.epoch:
                        raise Cancelled("Операция из очереди отменена запросом SAFE")
                    self.active_epoch = epoch
                result = self._dispatch(operation, args)
                future.set_result(result)
            except Exception as exc:
                if operation in {"script", "update"}:
                    self._job(status="cancelled" if isinstance(exc, Cancelled) else "failed", message=str(exc), deadline=None)
                self.event("error", str(exc))
                with self.lock:
                    self.state["last_error"] = str(exc)
                future.set_exception(exc)
            finally:
                self.active_epoch = None

    def close(self):
        self.stop.set()
        self.cancel.set()
        self.answer.set()
        self.worker.join(timeout=4)
        self._close("Сервер остановлен")
        self.log.close()
