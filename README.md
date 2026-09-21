# contr-fw — прошивка устройства стенда CONTR

Прошивка платы CONTR (NUCLEO-F767ZI), контроллера нагрузок (NUCLEO-G431KB), эмулятор и
ручной интерфейс. Требования — ТЗ [spec/19](https://github.com/Fe0dor/checker/blob/main/spec/19-bench-controller-firmware.md)
репозитория платформы, интерфейс — [spec/21](https://github.com/Fe0dor/checker/blob/main/spec/21-contr-manual-interface.md),
порядок работ — [spec/22](https://github.com/Fe0dor/checker/blob/main/spec/22-contr-firmware-plan.md).
Описание платы — [docs/hardware/contr.md](https://github.com/Fe0dor/checker/blob/main/docs/hardware/contr.md) там же.

## Дерево

| Каталог | Что |
|---|---|
| `hal/` | интерфейс железа `hal.h`; `hal/target` — STM32, `hal/host` — заглушки с виртуальным временем (А2) |
| `core/` | логика прошивки, компилируется на хосте без изменений (протокол, обновление, реле и блокировки) |
| `app/` | `main.c` платы |
| `gen/` | сгенерированное из единого источника — не править руками (FW-239) |
| `board/rev1/` | `relays.yaml` — единый источник таблицы реле; скрипт компоновки |
| `tools/` | `gen_board.py` генератор, `flash.py` загрузка через J-Link, `console.py` консоль USART3 |
| `tests/host/` | тесты C для `ctest`; `tests/py/` — pytest генератора и интерфейса |
| `ui/` | браузерный пульт `contr_ui` (панели, отдельный терминал, pyserial) |
| `emu/` | эмулятор `contr_emu` — с шага 2 |
| `loadboard/` | прошивка контроллера нагрузок — с шага 7 |
| `dialogs/` | эталонные диалоги «команда → ответ» — с шага 2 |
| `third_party/` | подмодули ST: CMSIS Core, CMSIS Device F7/G4, HAL F7/G4 |
| `docs/protocols/` | протоколы проверки шагов на железе |

## Установка

Нужны: CMake ≥ 3.25, Ninja, Arm GNU Toolchain (`arm-none-eabi-gcc`), хост-компилятор
(Clang на Mac, MinGW-w64 GCC на Windows), Python ≥ 3.12, SEGGER J-Link Software
(в winget он опубликован как `NordicSemiconductor.JLink` — тот же установщик SEGGER).

**Windows** (winget, после установки перезапустить терминал):

```powershell
winget install --id Kitware.CMake --id Ninja-build.Ninja --id Arm.GnuArmEmbeddedToolchain --id BrechtSanders.WinLibs.POSIX.UCRT --id NordicSemiconductor.JLink
```

**Mac** (Homebrew; Clang идёт с Xcode Command Line Tools):

```bash
xcode-select --install
brew install cmake ninja
brew install --cask gcc-arm-embedded segger-jlink
```

**Общее:**

```bash
git clone --recurse-submodules https://github.com/Fe0dor/contr-fw.git
cd contr-fw
python -m pip install -r requirements-dev.txt
python -m pip install -e ui
python -m pip install -e emu
```

Отладчик NUCLEO должен быть перепрошит в J-Link OB
([SEGGER, «ST-LINK on-board»](https://www.segger.com/products/debug-probes/j-link/models/other-j-links/st-link-on-board/)).

## Сборка и проверка

Одна команда на сборку — рабочие пресеты CMake:

```bash
cmake --workflow --preset host      # логика + ctest на хосте
cmake --workflow --preset target    # образ build/target/contr-fw.{elf,hex,bin}
python -m pytest                    # генератор и интерфейс
python tools/gen_board.py --check   # gen/ соответствует relays.yaml
cmake --workflow --preset bringup   # наладочная прошивка с DBG:*
python tools/run_dialogs.py --host build/host/contr-host.exe  # Windows; без .exe на Mac
python tools/run_dialogs.py --emu
```

На Windows, если в системе есть и MSVC, и GCC, задайте `CC=gcc` перед пресетом `host`.

## Плата

```bash
python tools/setup_dual_loader.py   # загрузчик SEGGER для двухбанковой flash (один раз)
python tools/flash.py               # загрузить build/target/contr-fw.hex через J-Link, сброс, пуск
python tools/console.py             # консоль USART3, автопоиск COM-порта J-Link / ST-LINK, 115200
python -m contr_ui                 # локальный пульт в Google Chrome
```

Версия 0.3.0 реализует ядро протокола и управление реле шагов 2–3. Рабочая сборка не содержит `DBG:*`;
для наладки выберите `build/bringup/contr-fw.hex`. Приёмка шагов 1–2 ещё открыта:
см. `docs/protocols/step-1.md` и `step-2.md`.

Запуск эмулятора: `python -m contr_emu --port 5025` (localhost).
Интерфейс: `python -m contr_ui`. Откроется Google Chrome: `http://127.0.0.1:8765`.
В панели «Подключение и сеть» выберите плату TCP, эмулятор или COM.
Для эмулятора по умолчанию указан порт 15026: запустите `python -m contr_emu --port 15026`.
Терминал можно открыть отдельно: `http://127.0.0.1:8765/terminal`.
Оба окна используют один канал устройства и общий журнал.
Панели: обзор, подключение/сеть, диагностика, конфигурация, сценарии, обновление и журнал.
Карта реле пока справочная: управление реле относится к шагу 3.
Пульт слушает только loopback; остановка сервера — Ctrl+C.
Для ручного открытия: `python -m contr_ui --no-browser`; другой HTTP-порт: `--port 8766`.
Интерфейс пишет журнал в `logs/`, опрашивает ошибки и события; период по умолчанию 500 мс.
Tab дополняет команды и имена из `INTERLOCK:LIST?`, стрелки переключают историю.
Файлы сценариев содержат команды, комментарии, `#wait <мс>` и `#prompt <действие>`.
Первая ошибка останавливает сценарий. SAFE всегда доступен отдельно.
Мастер принимает `.upd`, показывает прогресс, переподключается после COMMIT и подтверждает
пробный запуск. «Не подтверждать» оставляет таймаут для проверки отката.
На плате обновление разрешается только после проверки опционных байтов.

## Единый источник

`board/rev1/relays.yaml` → `python tools/gen_board.py` → `gen/`:
`relay_table.{h,c}` для прошивки, `interlock-table.json` для пакета плагина (схема SDK,
попарная развёртка групп, без `L*`), `relay-numbers.json` для эмулятора и интерфейса,
`interlock-groups.md` для документации. Контрольная сумма — `sha256` канонического JSON
таблицы; примечания и комментарии на неё не влияют.

## CI

`.github/workflows/ci.yml`: на `macos-latest` и `windows-latest` — проверка генерации,
pytest, `host` (сборка + ctest), `target` (сборка образа, hex в артефактах).


## Реле — шаг 3

В Chrome откройте `http://127.0.0.1:8765/#relays`. Карта из `INTERLOCK:LIST?`
разбита по блокам платы; кнопка реле включает или выключает его, зелёный показывает
последний записанный образ. Есть поиск, фильтр групп, точный список `ROUT:SET` и
выключение всех реле. `Tab` дополняет последнее имя в списке. В конфликте показаны
группа и оба имени; изменения не применяются. На странице «Команды» теперь 22 рабочие
команды и 10 наладочных. Состояние реле не является измерением контактов.

Для локального полного сравнения C и эмулятора с файлом блокировок:

```sh
python tools/check_relay_contract.py --host build/host/contr-host
# Windows: build/host/contr-host.exe
# Дополнительно --sdk-src ../checker/src для реального InterlockTable.violated_group().
```

Проверяются все 609 пар и все 81 имя. Эта утилита запускает локальный хост-процесс.
Диалог `dialogs/step3-relays.txt` предназначен также для платы: внешние цепи должны
быть отключены, а петля SR0 подключена. Подробности и незакрытая измерительная
приёмка: [протокол шага 3](docs/protocols/step-3.md).


## Шаг 4: секции и сопротивления

Прошивка/эмулятор/пульт 0.4.0 добавляют `RES:PWR`, `RES:SET`, `RES:STAT?`.
В пульте откройте «Секции и сопротивления»: питание A/B, десять элементов и
помощник ручных измерений с экспортом CSV. Точки сохраняются в браузере;
выгружайте CSV для хранения калибровки. Омметр к пульту автоматически не подключается.
Команды шага 4 выделены в справочнике отдельной группой.

Программные проверки завершены; измерения и физический обрыв I²C ожидают владельца
у стенда. Состояние и список измерений: [протокол шага 4](docs/protocols/step-4.md).
