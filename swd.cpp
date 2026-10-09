// =============================================================================
//  swd.cpp -- низкоуровневый bit-bang SWD для ESP32
// =============================================================================
#include <Arduino.h>
#include "config.h"
#include "app_hooks.h"
#include "swd.h"

// Прямой доступ к регистрам GPIO (быстро и без накладных расходов digitalWrite).
// GPIO.out_w1ts / out_w1tc  -- установка/сброс выходов одной записью
// GPIO.enable_w1ts / w1tc    -- включение/выключение драйвера (Z-состояние)
// GPIO.in                    -- чтение уровней
#include "soc/gpio_struct.h"

#define SWCLK_MASK      (1u << PIN_SWCLK)
#define SWDIO_MASK      (1u << PIN_SWDIO)

#define SWCLK_HIGH()    (GPIO.out_w1ts    = SWCLK_MASK)
#define SWCLK_LOW()     (GPIO.out_w1tc    = SWCLK_MASK)
#define SWDIO_HIGH()    (GPIO.out_w1ts    = SWDIO_MASK)
#define SWDIO_LOW()     (GPIO.out_w1tc    = SWDIO_MASK)
#define SWDIO_DRIVE()   (GPIO.enable_w1ts = SWDIO_MASK)
#define SWDIO_FLOAT()   (GPIO.enable_w1tc = SWDIO_MASK)
#define SWDIO_LEVEL()   (((GPIO.in) & SWDIO_MASK) != 0)

// ---------------------------------------------------------------------------
//  Тактирование.  Один "такт" SWD = низкий уровень, затем высокий.
//  Цель выдаёт новый бит по одному фронту SWCLK и сэмплирует по другому,
//  поэтому: при записи бита данные выставляются ДО спада, а при чтении
//  сэмплируем в низкой фазе (так делают DAPLink/ST-Link).
// ---------------------------------------------------------------------------
static volatile uint8_t s_nops = SWD_SPEED_DEFAULT;   // задержка полутакта (итераций NOP)

static inline void swdDelay(void)
{
  uint32_t n = s_nops;
  while (n--) {
    __asm__ __volatile__("nop");
  }
}

static inline void clockPulse(void)   // один такт SWCLK
{
  SWCLK_LOW();
  swdDelay();
  SWCLK_HIGH();
  swdDelay();
}

static inline void writeBit(uint32_t b)
{
  if (b) { SWDIO_HIGH(); } else { SWDIO_LOW(); }
  clockPulse();
}

static inline uint32_t readBit(void)
{
  SWCLK_LOW();
  swdDelay();
  uint32_t b = SWDIO_LEVEL() ? 1u : 0u;
  SWCLK_HIGH();
  swdDelay();
  return b;
}

// Данные и запросы передаются младшим битом вперёд (LSB first)
static inline void writeBits(uint32_t v, uint8_t n)
{
  for (uint8_t i = 0; i < n; i++) {
    writeBit(v & 1u);
    v >>= 1;
  }
}

static inline uint32_t readBits(uint8_t n)
{
  uint32_t v = 0;
  for (uint8_t i = 0; i < n; i++) {
    v |= (readBit() << i);
  }
  return v;
}

// Нечётный паритет по 32 битам данных
static inline uint32_t parity32(uint32_t v)
{
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return v & 1u;
}

// Пакет запроса SWD: START | APnDP | RnW | A2 | A3 | PARITY | STOP | PARK
static inline uint8_t makeRequest(bool ap, bool read, uint8_t reg)
{
  uint8_t a2 = (reg >> 2) & 1u;
  uint8_t a3 = (reg >> 3) & 1u;
  uint8_t p  = (uint8_t)((ap ? 1u : 0u) ^ (read ? 1u : 0u) ^ a2 ^ a3);
  return (uint8_t)(0x81u | (ap ? 0x02u : 0u) | (read ? 0x04u : 0u) |
                   (a2 << 3) | (a3 << 4) | (p << 5));
}

// Idle-такты: линия прижата к нулю, хост владеет линией
static inline void idleCycles(uint8_t n)
{
  SWDIO_DRIVE();
  SWDIO_LOW();
  for (uint8_t i = 0; i < n; i++) {
    clockPulse();
  }
}

// Последовательность перехода JTAG -> SWD + line reset (как в ST-Link/OpenOCD):
//   1) >= 50 тактов SWDIO = 1  (line reset)
//   2) 16 тактов 0xE79E (LSB first) -- специальный JTAG-to-SWD код
//   3) >= 50 тактов SWDIO = 1  (line reset)
//   4) >= 2 такта SWDIO = 0    (idle)
static void swdSwitchToSwd(void)
{
  SWDIO_DRIVE();
  SWDIO_HIGH();
  for (int i = 0; i < 56; i++) clockPulse();

  writeBits(0xE79Eu, 16);

  SWDIO_HIGH();
  for (int i = 0; i < 56; i++) clockPulse();

  SWDIO_LOW();
  for (int i = 0; i < 8; i++) clockPulse();

  SWCLK_HIGH();          // SWCLK в idle-состоянии (высокий)
}

// ---------------------------------------------------------------------------
//  Одна попытка транзакции. Возвращает код ACK (SWD_ACK_*) или 0xFF при
//  ошибке паритета. Рекурсии нет -- безопасно вызывать из обработчика FAULT.
// ---------------------------------------------------------------------------
uint8_t Swd::attempt(bool ap, bool read, uint8_t reg, uint32_t *data)
{
  uint8_t req = makeRequest(ap, read, reg);

  SWDIO_DRIVE();
  writeBits(req, 8);
  SWDIO_FLOAT();
  clockPulse();                    // turnaround: хост -> цель

  uint8_t ack = (uint8_t)readBits(3);

  if (ack == SWD_ACK_OK) {
    if (read) {
      uint32_t v = readBits(32);
      uint32_t p = readBit();
      clockPulse();                // turnaround: цель -> хост
      SWDIO_DRIVE();
      idleCycles(2);
      if (p != parity32(v)) {
        _err = SWD_ERR_PARITY;
        return 0xFF;
      }
      *data = v;
    } else {
      uint32_t v = *data;
      clockPulse();                // turnaround: цель -> хост
      SWDIO_DRIVE();
      writeBits(v, 32);
      writeBit(parity32(v));
      idleCycles(2);
    }
  } else {
    // WAIT/FAULT/мусор -- корректно завершить транзакцию
    clockPulse();
    SWDIO_DRIVE();
    idleCycles(4);
  }
  return ack;
}

// ---------------------------------------------------------------------------
//  Транзакция с повтором при WAIT. Цель отвечает WAIT, когда занята --
//  например, флеш-контроллер STM32 программирует полуслово и держит шину AHB.
// ---------------------------------------------------------------------------
bool Swd::transfer(bool ap, bool read, uint8_t reg, uint32_t *data)
{
  uint32_t t0 = millis();

  for (;;) {
    if (appCancelRequested()) {
      _err = SWD_ERR_CANCELLED;
      return false;
    }
    uint8_t ack = attempt(ap, read, reg, data);

    if (ack == SWD_ACK_OK) {
      _err = SWD_ERR_NONE;
      return true;
    }
    if (ack == 0xFF) {
      return false;                       // ошибка паритета (в _err уже записана)
    }
    if (ack == SWD_ACK_WAIT) {
      _retries++;
      if ((millis() - t0) > SWD_WAIT_TIMEOUT_MS) {
        _err = SWD_ERR_WAIT_TIMEOUT;
        return false;
      }
      delayMicroseconds(50);
      continue;                           // повтор той же транзакции
    }
    // FAULT или некорректный ACK: сбросить sticky-ошибки и вернуть ошибку
    _err = SWD_ERR_ACK;
    uint32_t abort = 0x0000001Eu;         // STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR
    attempt(false, false, DP_ABORT, &abort);
    return false;
  }
}

// ---------------------------------------------------------------------------
//  Публичные обёртки
// ---------------------------------------------------------------------------
void Swd::begin()
{
  if (_nops == 0) {
    _nops = SWD_SPEED_DEFAULT;
  }
  s_nops = _nops;

  pinMode(PIN_SWCLK, OUTPUT);
  pinMode(PIN_SWDIO, INPUT);

  SWDIO_LOW();          // подготовить защёлку выхода
  SWDIO_FLOAT();        // линия свободна (её может тянуть цель)
  SWCLK_HIGH();

  _err = SWD_ERR_NONE;
}

void Swd::setSpeed(uint8_t nops)
{
  _nops = nops ? nops : 1;
  s_nops = _nops;
}

const char *Swd::errorText() const
{
  switch (_err) {
    case SWD_ERR_NONE:         return "нет ошибки";
    case SWD_ERR_ACK:          return "цель ответила FAULT (ошибка доступа к памяти)";
    case SWD_ERR_WAIT_TIMEOUT: return "цель слишком долго отвечает WAIT";
    case SWD_ERR_PARITY:       return "ошибка паритета на SWDIO";
    case SWD_ERR_CANCELLED:    return "операция отменена пользователем";
    default:                   return "неизвестная ошибка";
  }
}

bool Swd::dpRead(uint8_t reg, uint32_t *val)
{
  return transfer(false, true, reg, val);
}

bool Swd::dpWrite(uint8_t reg, uint32_t val)
{
  return transfer(false, false, reg, &val);
}

// Чтения AP "posted": результат текущего чтения приходит только на следующей
// транзакции, поэтому читаем AP-регистр, а затем забираем значение из RDBUFF.
bool Swd::apRead(uint8_t reg, uint32_t *val)
{
  uint32_t dummy = 0;
  if (!transfer(true, true, reg, &dummy)) return false;
  return transfer(false, true, DP_RDBUFF, val);
}

bool Swd::apWrite(uint8_t reg, uint32_t val)
{
  return transfer(true, false, reg, &val);
}

bool Swd::selectBank(uint8_t bank)
{
  // APSEL = 0 (единственный AP), APBANKSEL = bank
  return dpWrite(DP_SELECT, (uint32_t)(bank & 0x0F) << 4);
}

bool Swd::setCsw(uint32_t csw)
{
  if (!apWrite(AP_CSW, csw)) return false;

  // Контроль записи: на части целей поле Size в CSW может "не приняться"
  // (CSW остаётся прежним), и тогда 16-битные операции молча уходят в шину
  // как 32-битные. Читаем регистр обратно, при расхождении повторяем запись
  // и предупреждаем в лог один раз на пару значений.
  uint32_t rb = 0;
  if (apRead(AP_CSW, &rb) && (rb & 0x3Fu) == (csw & 0x3Fu)) return true;

  if (apWrite(AP_CSW, csw)) {
    rb = 0;
    if (apRead(AP_CSW, &rb) && (rb & 0x3Fu) == (csw & 0x3Fu)) return true;
  }

  static uint32_t s_warnedWant = 0xFFFFFFFFu;
  static uint32_t s_warnedGot   = 0xFFFFFFFFu;
  if (s_warnedWant != csw || s_warnedGot != rb) {
    s_warnedWant = csw;
    s_warnedGot   = rb;
    appLog("SWD: CSW: записано 0x%08X, прочитано 0x%08X (поле Size не применилось)\n",
           (unsigned)csw, (unsigned)rb);
  }
  return true;   // не фатально: операция выполнится с фактическим размером передачи
}

void Swd::clearSticky()
{
  uint32_t v = 0x0000001Eu;
  attempt(false, false, DP_ABORT, &v);
}

// ---------------------------------------------------------------------------
//  Подключение к цели: line reset -> power-up -> проверка AHB-AP
// ---------------------------------------------------------------------------
bool Swd::connect()
{
  _err     = SWD_ERR_NONE;
  _retries = 0;
  _idcode  = 0;
  _apidr   = 0;

  // Пробуем подключиться; если ответа нет -- снижаем скорость и пробуем снова
  for (int tries = 0; tries < 4; tries++) {
    swdSwitchToSwd();

    uint32_t id  = 0;
    uint8_t  ack = attempt(false, true, DP_IDCODE, &id);

    // У SW-DP ARM в битах [11:1] лежит код разработчика (0x23B = ARM)
    if (ack == SWD_ACK_OK && (((id >> 1) & 0x7FFu) == 0x23Bu)) {
      _idcode = id;
      break;
    }

    appLog("SWD: нет корректного IDCODE (ACK=0x%02X, данные=0x%08X)\n", ack, id);

    uint16_t slower = (uint16_t)_nops * 4u;
    if (slower > 250u) slower = 250u;
    setSpeed((uint8_t)slower);
    appLog("SWD: пробую медленнее (N=%u)\n", (unsigned)_nops);
    delay(5);
  }

  if (_idcode == 0) {
    _err = SWD_ERR_ACK;
    return false;
  }

  appLog("SWD: связь есть, DPIDR = 0x%08X\n", _idcode);

  // Сброс "залипших" ошибок, выбор AP0/bank0, запрос питания домена отладки
  clearSticky();
  dpWrite(DP_SELECT, 0x00000000u);

  dpWrite(DP_CTRLSTAT, 0x50000F00u);   // CSYSPWRUPREQ | CDBGPWRUPREQ (как DAPLink/ST-Link)

  bool powered = false;
  for (int i = 0; i < 50; i++) {
    uint32_t s = 0;
    if (dpRead(DP_CTRLSTAT, &s) && ((s & 0xA0000000u) == 0xA0000000u)) {
      powered = true;
      break;
    }
    delay(2);
  }
  if (!powered) {
    appLog("SWD: предупреждение: нет подтверждения power-up (CTRL/STAT)\n");
  }

  // Убеждаемся, что AP -- это MEM-AP (AHB-AP): читаем IDR в bank 0xF.
  // Для Cortex-M3 AHB-AP ожидается 0x24770011.
  selectBank(0x0F);
  uint32_t idr = 0;
  if (apRead(AP_BANK_REG3, &idr)) {
    _apidr = idr;
    appLog("SWD: AHB-AP IDR = 0x%08X\n", idr);
  } else {
    appLog("SWD: предупреждение: не удалось прочитать AHB-AP IDR\n");
  }
  selectBank(0x00);

  if (!setCsw(CSW_32BIT_INC)) {
    return false;
  }
  return true;
}

bool Swd::reconnect()
{
  return connect();
}

// ---------------------------------------------------------------------------
//  Доступ к памяти цели через MEM-AP
// ---------------------------------------------------------------------------
bool Swd::memRead32(uint32_t addr, uint32_t *val)
{
  if (!setCsw(CSW_32BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;
  return apRead(AP_DRW, val);
}

bool Swd::memWrite32(uint32_t addr, uint32_t val)
{
  if (!setCsw(CSW_32BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;
  return apWrite(AP_DRW, val);
}

bool Swd::memRead16(uint32_t addr, uint16_t *val)
{
  uint32_t v = 0;
  if (!setCsw(CSW_16BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;
  if (!apRead(AP_DRW, &v)) return false;
  *val = (uint16_t)(v & 0xFFFFu);
  return true;
}

bool Swd::memWrite16(uint32_t addr, uint16_t val)
{
  if (!setCsw(CSW_16BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;
  uint32_t v = val;
  return apWrite(AP_DRW, v);
}

bool Swd::memWriteHalfwords(uint32_t addr, const uint16_t *data, uint32_t count)
{
  if (count == 0) return true;

  // ТОЛЬКО 16-битные транзакции: 32-битная запись в флеш STM32F1 отклоняется
  // контроллером (AHB отвечает ошибкой -> ACK=FAULT на SWD).
  //
  // ВАЖНО: на данной цели AHB-AP не разводит данные по адресным дорожкам
  // (byte lanes) -- при записи полуслова по адресу с (addr % 4 == 2) контроллер
  // флеша читает старшее полуслово WDATA, в котором у "сырого" нулевого
  // расширения стояли нули. Отсюда баг "во флеше 0x00 вместо данных" на
  // нечётных полусловах. Дублируем полуслово в обе части слова: нужные байты
  // оказываются в правильной дорожке в любом случае (и при обычной разводке
  // лишние байты попадают под запрет записи BE и не записываются).
  if (!setCsw(CSW_16BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t hw = data[i];
    uint32_t v  = hw | (hw << 16);      // полуслово продублировано в обе halves WDATA
    // TAR автоинкрементируется на 2 байта после каждой 16-битной записи
    if (!transfer(true, false, AP_DRW, &v)) return false;
    if ((i & 0x3Fu) == 0x3Fu && appCancelRequested()) {
      _err = SWD_ERR_CANCELLED;
      return false;
    }
  }
  return true;
}

bool Swd::memReadWords(uint32_t addr, uint32_t *data, uint32_t count)
{
  if (count == 0) return true;
  if (!setCsw(CSW_32BIT_INC)) return false;
  if (!apWrite(AP_TAR, addr)) return false;

  uint32_t v = 0;
  for (uint32_t i = 0; i < count; i++) {
    // Каждое чтение DRW возвращает результат ПРЕДЫДУЩЕГО чтения, поэтому
    // первое значение "мусорное", а последнее забираем из RDBUFF.
    if (!transfer(true, true, AP_DRW, &v)) return false;
    if (i > 0) data[i - 1] = v;
    if ((i & 0x3Fu) == 0x3Fu && appCancelRequested()) {
      _err = SWD_ERR_CANCELLED;
      return false;
    }
  }
  if (!dpRead(DP_RDBUFF, &v)) return false;
  data[count - 1] = v;
  return true;
}
