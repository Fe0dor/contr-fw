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
| `core/` | логика прошивки, компилируется на хосте без изменений (шаг 0: мигание и эхо) |
| `app/` | `main.c` платы |
| `gen/` | сгенерированное из единого источника — не править руками (FW-239) |
| `board/rev1/` | `relays.yaml` — единый источник таблицы реле; скрипт компоновки |
| `tools/` | `gen_board.py` генератор, `flash.py` загрузка через J-Link, `console.py` консоль USART3 |
| `tests/host/` | тесты C для `ctest`; `tests/py/` — pytest генератора и интерфейса |
| `ui/` | ручной интерфейс `contr_ui` (tkinter + pyserial) |
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
```

На Windows, если в системе есть и MSVC, и GCC, задайте `CC=gcc` перед пресетом `host`.

## Плата

```bash
python tools/flash.py               # загрузить build/target/contr-fw.hex через J-Link, сброс, пуск
python tools/console.py             # консоль USART3, автопоиск COM-порта J-Link / ST-LINK, 115200
python -m contr_ui --com COM5       # интерфейс: терминал, TCP или COM, кнопка SAFE
```

Образ шага 0 мигает LD1 (PB0) с периодом 1 с и возвращает эхом всё принятое по USART3.

## Единый источник

`board/rev1/relays.yaml` → `python tools/gen_board.py` → `gen/`:
`relay_table.{h,c}` для прошивки, `interlock-table.json` для пакета плагина (схема SDK,
попарная развёртка групп, без `L*`), `relay-numbers.json` для эмулятора и интерфейса,
`interlock-groups.md` для документации. Контрольная сумма — `sha256` канонического JSON
таблицы; примечания и комментарии на неё не влияют.

## CI

`.github/workflows/ci.yml`: на `macos-latest` и `windows-latest` — проверка генерации,
pytest, `host` (сборка + ctest), `target` (сборка образа, hex в артефактах).
