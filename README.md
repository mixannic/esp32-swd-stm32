# esp32-swd-stm32
# ESP32 SWD Programmer for STM32F103

[![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue.svg)](#)
[![Arduino Core: 2.x/3.x](https://img.shields.io/badge/arduino--core-2.x%20%7C%203.x-orange.svg)](#)
[![Target: STM32F1](https://img.shields.io/badge/target-STM32F1-green.svg)](#)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)](LICENSE)

**Language / Язык:** [English](#-english) · [Русский](#-русский)

---

## 📖 Table of Contents

- [English](#-english)
  - [Features](#-features)
  - [Hardware](#-hardware)
  - [Software](#-software)
  - [Usage](#-usage)
  - [How it works](#-how-it-works)
  - [HTTP API](#-http-api)
  - [Limitations](#-limitations)
  - [Troubleshooting](#-troubleshooting)
  - [Project layout](#-project-layout)
  - [Roadmap](#-roadmap)
  - [Contributing](#-contributing)
  - [License](#-license)
  - [Credits](#-credits)
- [Русский](#-русский)
  - [Возможности](#-возможности)
  - [Аппаратная часть](#-аппаратная-часть)
  - [Программная часть](#-программная-часть)
  - [Использование](#-использование)
  - [Как это работает](#-как-это-работает)
  - [HTTP API](#-http-api-1)
  - [Ограничения](#-ограничения)
  - [Диагностика](#-диагностика)
  - [Структура проекта](#-структура-проекта)
  - [Планы по развитию](#-планы-по-развитию)
  - [Вклад](#-вклад)
  - [Лицензия](#-лицензия)
  - [Благодарности](#-благодарности)

---

# 🇬🇧 English

> Turns an **ESP32** into a wireless programmer for **STM32F103** (and compatible STM32F1 chips) via **SWD**.
> Upload a `.bin` to the ESP32 and flash the target right from your browser — no ST-Link, no USB, no wires to your computer.

---

## ✨ Features

| Feature | Description |
|---|---|
| 🔌 **Fully software SWD** | Bit-bang on two GPIOs — no external chips |
| 🌐 **Web UI** | Upload `.bin`, pick speed, set flash options from the browser |
| ⚡ **STM32F1 flashing** | Via MEM-AP (AHB-AP): halt → HSI → unlock → erase → program → verify → lock → reset |
| 🎯 **Smart skipping** | Pages already matching the file are not rewritten |
| ✅ **Verification** | Optional read-back verification after programming |
| 🗑️ **Mass erase** | Full flash erase from the browser |
| 💾 **Flash dump** | Read target flash into a `.bin` file |
| 📊 **Flash status** | Total / used / free bytes of target flash |
| 📝 **Live log** | Real-time log in the web UI and on Serial |
| 🐢 **Auto slowdown** | Automatic SWD slowdown if the target doesn't respond |
| 📡 **mDNS** | Device reachable at `http://esp32-swd.local/` |

---

## 🔧 Hardware

### What you need

- **ESP32** — any board (DevKit, WROOM, WROVER)
- **STM32F103** — or another STM32F1: `F100`, `F102`, `F103`, `F105`, `F107`
- **4 wires** — for SWD without NRST

### Wiring

| ESP32 | STM32F103 | Note |
|:-----:|:---------:|:-----|
| `GPIO18` | `PA14` (SWCLK, "CLK") | SWD clock |
| `GPIO23` | `PA13` (SWDIO, "DIO") | SWD data |
| `GND` | `GND` | Common ground |
| `3V3` | `3V3` | ⚠️ **Only if the STM32 is NOT powered from USB!** |

> [!WARNING]
> Do **not** feed 3.3 V from the ESP32 into the STM32 if the STM32 is also connected to your computer's USB — you risk damaging one of the power sources.

### Optional: NRST

If the target firmware disables SWD (e.g. goes to sleep after reset, or repurposes PA13/PA14 as GPIO), you can wire the reset line.

Uncomment in `config.h`:

```c
#define PIN_NRST 5

Then connect GPIO5 to the STM32 RESET pin (add a 10 kΩ pull-up to 3.3 V if the board doesn't have one).
💻 Software
Requirements
Component	Version
Arduino IDE	2.x (or arduino-cli)
ESP32 Arduino Core	2.0.x or 3.x
Libraries	WiFi, WebServer, LittleFS, ESPmDNS (built-in)
Configuration

Open config.h and set your credentials:
c

#define WIFI_SSID     "your_SSID"
#define WIFI_PASS     "your_password"
#define WIFI_HOSTNAME "esp32-swd"   // mDNS name

Change pins and default speed if needed:
c

#define PIN_SWCLK 18
#define PIN_SWDIO 23

// 8 ≈ 2 MHz, 24 ≈ 1 MHz, 60 ≈ 400 kHz, 150 ≈ 150 kHz
#define SWD_SPEED_DEFAULT 24

Build and flash the ESP32
bash

# 1. Clone the repository
git clone https://github.com/mixannic/esp32-swd-stm32.git

# 2. Open ESP32_SWD_Programmer.ino in Arduino IDE
# 3. Select board: "ESP32 Dev Module"
# 4. Ensure partition scheme leaves room for LittleFS
#    (e.g. "Default 4MB with spiffs")
# 5. Compile and upload

    [!TIP]
    Open the serial monitor at 115200 baud — the boot log and IP address are printed there.

🚀 Usage

    Power up the ESP32. The serial monitor shows the IP.

    Open http://<IP>/ or http://esp32-swd.local/ in a browser.

    Upload a .bin — the file is stored in the ESP32's LittleFS.

    Click "Connect" — the ESP32 detects the target and shows DEV_ID, flash size, RDP.

    Optionally:

        "Mass erase" — erases the whole STM32 flash

        "Flash status" — shows used / free bytes

    Configure flash options:

        ⚡ Speed

        ✅ Verify after programming

        ⏭️ Skip matching pages

        🔄 Reset after flashing

    Click "Flash".

Progress, current stage, and the log are shown on the page in real time.
📥 Reading flash

The "Dump flash (.bin)" button reads the entire STM32 flash and saves it to your computer as:
text

stm32_flash_<size>kb.bin

    [!NOTE]
    RDP must be level 0 for reads to work. With read protection enabled, reads return zeros or garbage.

⚙️ How it works

The project has three layers:
1. swd.cpp / swd.h — low-level ARM Serial Wire Debug (ADIv5)

    JTAG-to-SWD switch (0xE79E) + line reset

    DP/AP transactions with parity and WAIT retries

    Debug domain power-up

    Memory access via AHB-AP (CSW/TAR/DRW + RDBUFF, posted reads)

2. stm32f1.cpp / stm32f1.h — STM32F1 operations

    Read DBGMCU_IDCODE, FLASH_SIZE, RDP option bytes

    Halt / run / reset core via DHCSR and AIRCR

    Unlock / lock flash controller with magic keys

    Page erase (PER+STRT) and mass erase (MER+STRT)

    Half-word programming (PG=1)

3. ESP32_SWD_Programmer.ino — the application

    HTTP server and API

    Jobs (connect / erase / flash / flashstat / read) in a dedicated FreeRTOS task

    Log, progress, cancel

    [!TIP]
    Details about STM32F1 registers are in the comments of stm32f1.cpp and stm32f1.h.

🌐 HTTP API
Method	Path	Description
GET	/	Web UI HTML page
GET	/api/status	JSON: job state, target, firmware, flash status, log
POST	/api/upload	Upload .bin (multipart/form-data, field firmware)
POST	/api/connect	Connect to target, read info
POST	/api/erase	Mass erase target flash
POST	/api/flashstat	Compute flash status (used / free)
POST	/api/flash	Flash the uploaded .bin
POST	/api/cancel	Cancel the current operation
GET	/api/readflash	Dump STM32 flash to .bin
Query parameters for /api/flash
Parameter	Values	Meaning
verify	0 / 1	Verify after programming
skip	0 / 1	Skip pages already matching the file
reset	0 / 1	Reset and run the target after flashing
speed	1..250	SWD half-cycle delay (NOP iterations; smaller = faster)
⚠️ Limitations
Limitation	Details
STM32F1 only	F100/F102/F103/F105/F107. F4/F0/G0/L4 need a different algorithm
.bin only	Not .hex, not .elf — see conversion below
Max 128 KB	MAX_FIRMWARE_SIZE in config.h. For F103xE (512 KB), raise it
2.4 GHz Wi-Fi only	The ESP32 doesn't support 5 GHz
No RDP level 1/2	Target blocks flash reads; remove protection first
Speed depends on wires	Start with "normal (~1 MHz)", drop to "slow" if unstable
Converting .elf → .bin
bash

arm-none-eabi-objcopy -O binary firmware.elf firmware.bin

🩺 Troubleshooting
Symptom	Fix
no SWD connection	Check GND, 3.3 V power, pin correctness; try "very slow" speed
target replied FAULT	Often RDP enabled or HSI off; ensure the target isn't asleep
failed to unlock flash controller	Press RESET on the target or reconnect
WARNING! Read protection is enabled	Remove RDP via ST-Link Utility / CubeProgrammer
Wi-Fi won't connect	Confirm SSID is 2.4 GHz; check password; move closer to the router
File larger than allowed	Raise MAX_FIRMWARE_SIZE and enlarge the LittleFS partition
📁 Project layout
text

.
├── ESP32_SWD_Programmer.ino   # Entry point: setup/loop, HTTP handlers, tasks
├── config.h                   # Settings: Wi-Fi, pins, speed, sizes
├── app_hooks.h                # Interface between .ino and modules (log/progress/cancel)
├── swd.h / swd.cpp            # Low-level bit-bang SWD (ADIv5)
├── stm32f1.h / stm32f1.cpp    # STM32F1 flash controller operations
├── web_ui.h                   # Web UI HTML/CSS/JS (in PROGMEM)
└── README.md

🗺️ Roadmap

    □

    Move flash reading into a dedicated task so UI and cancel work during dump
    □

    Replace String with fixed buffers (reduce heap fragmentation over long runs)
    □

    Support STM32F4 / G0 / L4
    □

    Flash .hex and .elf (parse on the ESP32 side)
    □

    OTA update of the ESP32 firmware via web UI
    □

    Watchdog and automatic Wi-Fi reconnect

🤝 Contributing

PRs and issues are welcome. If you found a bug or added support for a new STM32 family — open a pull request.

    Fork the repository

    Create a feature branch: git checkout -b feature/my-feature

    Commit your changes: git commit -am 'Add some feature'

    Push the branch: git push origin feature/my-feature

    Open a Pull Request

📄 License

Distributed under the MIT License. See LICENSE for details.
🙏 Credits

    ARM Debug Interface v5 Architecture Specification — SWD description

    RM0008 — STM32F1 Reference Manual, FLASH controller section

    OpenOCD and DAPLink — reference SWD sequences

🇷🇺 Русский

    Превращает ESP32 в беспроводной программатор STM32F103 (и совместимых STM32F1) по протоколу SWD.
    Загрузка .bin в ESP32 и прошивка цели происходят через браузер — без ST-Link, без USB, без проводов к компьютеру.

✨ Возможности
Возможность	Описание
🔌 Полностью программный SWD	Bit-bang на двух GPIO — никаких внешних микросхем
🌐 Веб-интерфейс	Загрузка .bin, выбор скорости, опции прошивки
⚡ Прошивка STM32F1	Через MEM-AP (AHB-AP): halt → HSI → unlock → erase → program → verify → lock → reset
🎯 Умный пропуск	Страницы, совпадающие с файлом, не перезаписываются
✅ Верификация	Опциональная проверка чтением после записи
🗑️ Массовое стирание	Полное стирание флеша из браузера
💾 Чтение флеша	Выгрузка содержимого флеша цели в .bin
📊 Статус флеша	Всего / занято / свободно (в байтах)
📝 Живой лог	Лог в реальном времени в веб-интерфейсе и Serial
🐢 Автозамедление	Автоматическое снижение скорости SWD при отсутствии отклика
📡 mDNS	Доступ по http://esp32-swd.local/
🔧 Аппаратная часть
Что нужно

    ESP32 — любая плата (DevKit, WROOM, WROVER)

    STM32F103 — или другой STM32F1: F100, F102, F103, F105, F107

    4 провода — для SWD без NRST

Подключение
ESP32	STM32F103	Комментарий
GPIO18	PA14 (SWCLK, «CLK»)	Тактирование SWD
GPIO23	PA13 (SWDIO, «DIO»)	Данные SWD
GND	GND	Общий провод
3V3	3V3	⚠️ Только если STM32 не питается от USB!

    [!WARNING]
    Не подавайте 3.3 В с ESP32 на STM32, если STM32 одновременно подключён к USB-порту компьютера — рискуете спалить один из источников питания.

Опционально: NRST

Если прошивка цели отключает SWD (например, сразу после сброса уходит в sleep или переводит PA13/PA14 в GPIO), можно подключить линию сброса.

Раскомментируйте в config.h:
c

#define PIN_NRST 5

Затем соедините GPIO5 с пином RESET STM32 (плюс подтяжка 10 кОм к 3.3 В, если её нет на плате).
💻 Программная часть
Требования
Компонент	Версия
Arduino IDE	2.x (или arduino-cli)
ESP32 Arduino Core	2.0.x или 3.x
Библиотеки	WiFi, WebServer, LittleFS, ESPmDNS (встроенные)
Настройка

Откройте config.h и укажите свои данные:
c

#define WIFI_SSID     "ваш_SSID"
#define WIFI_PASS     "ваш_пароль"
#define WIFI_HOSTNAME "esp32-swd"   // mDNS-имя

При необходимости поменяйте пины и скорость по умолчанию:
c

#define PIN_SWCLK 18
#define PIN_SWDIO 23

// 8 ≈ 2 МГц, 24 ≈ 1 МГц, 60 ≈ 400 кГц, 150 ≈ 150 кГц
#define SWD_SPEED_DEFAULT 24

Сборка и прошивка ESP32
bash

# 1. Склонируйте репозиторий
git clone https://github.com/<ваш-логин>/esp32-swd-stm32.git

# 2. Откройте ESP32_SWD_Programmer.ino в Arduino IDE
# 3. Выберите плату: "ESP32 Dev Module"
# 4. Убедитесь, что размер Flash-раздела позволяет LittleFS
#    (схема "Default 4MB with spiffs" или аналогичная)
# 5. Скомпилируйте и загрузите

    [!TIP]
    В Arduino IDE откройте монитор порта на 115200 бод — там будет лог загрузки и IP-адрес.

🚀 Использование

    Подайте питание на ESP32. В Serial-мониторе появится IP-адрес.

    Откройте http://<IP>/ или http://esp32-swd.local/ в браузере.

    Загрузите .bin — файл сохранится в LittleFS ESP32.

    Нажмите «Подключиться» — ESP32 определит цель и покажет DEV_ID, размер флеша, RDP.

    При необходимости:

        «Массовое стирание» — сотрёт весь флеш STM32

        «Статус флеша» — покажет, сколько занято / свободно

    Настройте опции прошивки:

        ⚡ Скорость

        ✅ Верификация после записи

        ⏭️ Пропуск совпадающих страниц

        🔄 Сброс после прошивки

    Нажмите «Прошить».

Прогресс, текущий этап и лог отображаются на странице в реальном времени.
📥 Чтение флеша

Кнопка «Выгрузить флеш (.bin)» читает весь флеш STM32 и сохраняет его на компьютер как:
text

stm32_flash_<размер>kb.bin

    [!NOTE]
    Для чтения флеша RDP должен быть уровня 0. При включённой защите чтение вернёт нули или мусор.

⚙️ Как это работает

Проект состоит из трёх слоёв:
1. swd.cpp / swd.h — низкоуровневый ARM Serial Wire Debug (ADIv5)

    JTAG-to-SWD switch (0xE79E) + line reset

    Транзакции DP/AP с паритетом и повторами при WAIT

    Power-up домена отладки

    Доступ к памяти цели через AHB-AP (CSW/TAR/DRW + RDBUFF, posted reads)

2. stm32f1.cpp / stm32f1.h — операции с STM32F1

    Чтение DBGMCU_IDCODE, FLASH_SIZE, RDP option bytes

    Halt / run / reset ядра через DHCSR и AIRCR

    Unlock / lock флеш-контроллера ключами

    Стирание страниц (PER+STRT) и массовое стирание (MER+STRT)

    Программирование полусловами (PG=1)

3. ESP32_SWD_Programmer.ino — приложение

    Веб-сервер и HTTP API

    Задания (connect / erase / flash / flashstat / read) в отдельной задаче FreeRTOS

    Лог, прогресс, отмена

    [!TIP]
    Подробности по регистрам STM32F1 — в комментариях к stm32f1.cpp и stm32f1.h.

🌐 HTTP API
Метод	Путь	Описание
GET	/	HTML-страница веб-интерфейса
GET	/api/status	JSON: состояние задания, цель, прошивка, статус флеша, лог
POST	/api/upload	Загрузка .bin (multipart/form-data, поле firmware)
POST	/api/connect	Подключиться к цели, прочитать информацию
POST	/api/erase	Массовое стирание флеша
POST	/api/flashstat	Рассчитать статус флеша (занято / свободно)
POST	/api/flash	Прошить загруженный .bin
POST	/api/cancel	Отменить текущую операцию
GET	/api/readflash	Выгрузить содержимое флеша STM32 в .bin
Параметры /api/flash
Параметр	Значения	Смысл
verify	0 / 1	Верификация после записи
skip	0 / 1	Пропускать страницы, совпадающие с файлом
reset	0 / 1	Сбросить и запустить цель после прошивки
speed	1..250	Задержка полутакта SWD (NOP-итераций; меньше = быстрее)
⚠️ Ограничения
Ограничение	Детали
Только STM32F1	F100/F102/F103/F105/F107. Для F4/F0/G0/L4 нужен другой алгоритм
Только .bin	Не .hex, не .elf — см. конвертацию ниже
Максимум 128 КБ	MAX_FIRMWARE_SIZE в config.h. Для F103xE (512 КБ) — увеличьте
Только 2.4 ГГц Wi-Fi	ESP32 не поддерживает 5 ГГц
RDP уровня 1/2 не работает	Цель блокирует чтение флеша; сначала снимите защиту
Скорость зависит от проводов	Начните с «нормально (~1 МГц)», при сбоях — «медленно»
Конвертация .elf → .bin
bash

arm-none-eabi-objcopy -O binary firmware.elf firmware.bin

🩺 Диагностика
Симптом	Что делать
нет связи по SWD	Проверьте GND, питание 3.3 В, правильность пинов; попробуйте «очень медленно»
цель ответила FAULT	Часто RDP включён или нет HSI; проверьте, что цель не в sleep
не удалось разблокировать флеш-контроллер	Сделайте сброс цели кнопкой RESET или переподключитесь
ВНИМАНИЕ! Включена защита от чтения	Снимите RDP через ST-Link Utility / CubeProgrammer
Wi-Fi не подключается	Убедитесь, что SSID 2.4 ГГц; проверьте пароль; поднесите ближе к роутеру
Файл больше допустимого	Увеличьте MAX_FIRMWARE_SIZE и размер раздела LittleFS
📁 Структура проекта
text

.
├── ESP32_SWD_Programmer.ino   # Точка входа: setup/loop, HTTP-хендлеры, задачи
├── config.h                   # Настройки: Wi-Fi, пины, скорость, размеры
├── app_hooks.h                # Интерфейс между .ino и модулями (лог/прогресс/отмена)
├── swd.h / swd.cpp            # Низкоуровневый bit-bang SWD (ADIv5)
├── stm32f1.h / stm32f1.cpp    # Операции с флеш-контроллером STM32F1
├── web_ui.h                   # HTML/CSS/JS веб-интерфейса (в PROGMEM)
└── README.md

🗺️ Планы по развитию

    □

    Перенос чтения флеша в отдельную задачу, чтобы UI и отмена работали во время выгрузки
    □

    Замена String на фиксированные буферы (снижение фрагментации кучи при долгой работе)
    □

    Поддержка STM32F4 / G0 / L4
    □

    Прошивка .hex и .elf (парсинг на стороне ESP32)
    □

    OTA-обновление прошивки ESP32 через веб-интерфейс
    □

    Watchdog и автоматический реконнект Wi-Fi

🤝 Вклад

PR и issue приветствуются. Если нашли баг или добавили поддержку нового семейства STM32 — открывайте pull request.

    Форкните репозиторий

    Создайте ветку: git checkout -b feature/my-feature

    Закоммитьте изменения: git commit -am 'Add some feature'

    Запушьте: git push origin feature/my-feature

    Откройте Pull Request

📄 Лицензия

Распространяется под лицензией MIT. Подробности — в файле LICENSE.
🙏 Благодарности

    ARM Debug Interface v5 Architecture Specification — за описание SWD

    RM0008 — Reference Manual для STM32F1, раздел про FLASH-контроллер

    OpenOCD и DAPLink — за эталонные последовательности SWD

<div align="center">

⭐ Если проект оказался полезен — поставьте звезду на GitHub! ⭐
</div> ```
Что изменилось по сравнению с прошлой версией
Приём	Где применён
🏷️ Бейджи (Shields.io)	В шапке — платформа, версия ядра, цель, лицензия
📑 Оглавление с якорями	Сразу после бейджей, для обеих языковых версий
➡️ Блочные цитаты-вступления	Под каждым # 🇬🇧 English / # 🇷🇺 Русский
🔣 Emoji в заголовках	Для визуальной навигации (✨, 🔧, 💻, 🚀, ⚙️, 🌐, ⚠️, 🩺, 📁, 🗺️, 🤝, 📄, 🙏)
📊 Таблицы с выравниванием (:---:, :---)	Для пинов, параметров API, ограничений, диагностики
💡 Callout-блоки GitHub (> [!WARNING], > [!TIP], > [!NOTE])	Для важных предупреждений и подсказок
🎨 Подсветка синтаксиса в code blocks	c, bash вместо голых блоков
🗂️ Вложенные подразделы через ###	В разделах «How it works», «HTTP API»
✅ Чек-листы	В разделе «Roadmap»
🔢 Нумерованные шаги для PR	В разделе «Contributing»
🎯 Футер с центрированием	Через <div align="center"> внизу
🏷️ Inline-код (`GPIO18`, `PA13`)	Везде, где упоминаются пины, регистры, пути, команды

