// =============================================================================
//  swd.h -- программная реализация протокола ARM Serial Wire Debug (ADIv5)
//           на двух GPIO ESP32 (SWCLK/SWDIO), "bit-bang".
//
//  Что реализовано (по мотивам того, как работает ST-Link и как устроен
//  OpenOCD / DAPLink / embedded-swd):
//    * JTAG-to-SWD switch sequence + line reset (0xE79E);
//    * транзакции DP/AP: 8 бит запроса, 1 такт turnaround, 3 бита ACK,
//      32 бита данных + parity, 1 такт turnaround, idle-такты;
//    * повторы при ответе WAIT (цель занята, например флеш-контроллер пишет);
//    * power-up домена отладки (DP CTRL/STAT = 0x50000F00) и ABORT=0x1E;
//    * доступ к памяти цели через MEM-AP (AHB-AP): CSW/TAR/DRW + RDBUFF
//      (posted reads), пакетные чтение/запись с автоинкрементом TAR.
// =============================================================================
#pragma once
#include <stdint.h>

// ---- коды ответа (ACK) ------------------------------------------------------
#define SWD_ACK_OK      0x1u
#define SWD_ACK_WAIT    0x2u
#define SWD_ACK_FAULT   0x4u

// ---- регистры Debug Port (индекс A[3:2]) -----------------------------------
#define DP_IDCODE       0x00    // R
#define DP_ABORT        0x00    // W
#define DP_CTRLSTAT     0x04    // R/W
#define DP_SELECT       0x08    // W
#define DP_RDBUFF       0x0C    // R

// ---- регистры Access Port (bank 0 для AHB-AP) ------------------------------
#define AP_CSW          0x00
#define AP_TAR          0x04
#define AP_DRW          0x0C
#define AP_BANK_REG3    0x0C    // 0xFC в bank 0xF -- IDR

// ---- значения CSW (Control and Status Word) --------------------------------
// 0x23000000 -- HPROT/MSTRTYPE (как в DAPLink/OpenOCD/embedded-swd)
// бит 6 DeviceEn = 1, биты [5:4] AddrInc = 01 (инкремент на размер пересылки)
// биты [2:0] Size: 001 = 16 бит, 010 = 32 бита
#define CSW_32BIT_INC   0x23000052u
#define CSW_16BIT_INC   0x23000051u

// ---- коды ошибок -----------------------------------------------------------
enum SwdError : uint8_t {
  SWD_ERR_NONE = 0,
  SWD_ERR_ACK,            // неверный ACK
  SWD_ERR_WAIT_TIMEOUT,   // цель слишком долго отвечает WAIT
  SWD_ERR_PARITY,
  SWD_ERR_CANCELLED,
};

class Swd {
public:
  // Настройка пинов + скорость (задержка полутакта, итераций NOP)
  void begin();
  void setSpeed(uint8_t nops);
  uint8_t speed() const { return _nops; }

  // Полное подключение: перевод цели в SWD, сброс линии, power-up, чтение IDCODE
  bool connect();
  // Быстрое переподключение (после сброса цели)
  bool reconnect();

  uint32_t idcode() const { return _idcode; }
  uint32_t apIdr()  const { return _apidr;  }
  uint8_t  lastError() const { return _err; }
  const char *errorText() const;
  uint32_t retryCount() const { return _retries; }

  // ---- транзакции -----------------------------------------------------------
  bool dpRead(uint8_t reg, uint32_t *val);
  bool dpWrite(uint8_t reg, uint32_t val);
  bool apRead(uint8_t reg, uint32_t *val);
  bool apWrite(uint8_t reg, uint32_t val);

  // ---- память цели через MEM-AP ---------------------------------------------
  bool setCsw(uint32_t csw);
  bool memRead32(uint32_t addr, uint32_t *val);
  bool memWrite32(uint32_t addr, uint32_t val);
  bool memRead16(uint32_t addr, uint16_t *val);
  bool memWrite16(uint32_t addr, uint16_t val);
  // Пакетная запись массива данных (используется для записи флеша STM32F1,
  // программирование полусловами через 16-битные транзакции; полуслово
  // продублировано в обоих половинах WDATA из-за особенности byte-lane
  // этой цели -- см. реализацию в swd.cpp).
  bool memWriteHalfwords(uint32_t addr, const uint16_t *data, uint32_t count);
  // Пакетное чтение 32-битных слов (конвейерное, через RDBUFF)
  bool memReadWords(uint32_t addr, uint32_t *data, uint32_t count);

  // Сброс "залипших" ошибок DP
  void clearSticky();

private:
  uint8_t attempt(bool ap, bool read, uint8_t reg, uint32_t *data);
  bool transfer(bool ap, bool read, uint8_t reg, uint32_t *data);
  bool selectBank(uint8_t bank);

  uint8_t  _nops    = 0;
  uint8_t  _err     = SWD_ERR_NONE;
  uint32_t _idcode  = 0;
  uint32_t _apidr   = 0;
  uint32_t _retries = 0;
};
