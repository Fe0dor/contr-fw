# emu — эмулятор устройства CONTR

Появляется на шаге 2 плана 22: пакет `contr_emu`, `python -m contr_emu` слушает TCP 5025
и исполняет тот же протокол и ту же таблицу блокировок из `gen/relay-numbers.json`
(FW-248…FW-250). Контракт для платформы: `Emulator()` без аргументов,
`supports_nonblocking_poll = True`, `read(timeout_s=0)` без блокировки (FW-249).
