// =============================================================================
//  stm32f1.h -- операции с STM32F1 через SWD: останов ядра, разблокировка
//               флеш-контроллера, стирание страниц, программирование
//               полусловами (как это делает ST-Link / OpenOCD / stm32f1x.c)
//
//  Карта регистров (RM0008, PM0075):
//    FLASH_KEYR 0x40022004   ключи 0x45670123 / 0xCDEF89AB
//    FLASH_SR   0x4002200C   BSY=0, PGERR=2, WRPRTERR=4, EOP=5
//    FLASH_CR   0x40022010   PG=0, PER=1, MER=2, OPTPG=4, OPTER=5, STRT=6, LOCK=7
//    FLASH_AR   0x40022014   адрес стираемой страницы
//  ВАЖНО: у STM32F1 бит STRT = 6 (в отличие от STM32F4, где он 16).
// =============================================================================
#pragma once
#include <stdint.h>
#include "swd.h"

struct Stm32F1Info {
  bool     valid    = false;
  uint32_t idcode   = 0;        // DPIDR (ожидается 0x1BA01477)
  uint32_t apIdr    = 0;        // AHB-AP IDR (ожидается 0x24770011)
  uint32_t dbgmcuId = 0;        // DBGMCU_IDCODE (0xE0042000)
  uint16_t devId    = 0;        // DEV_ID[11:0]
  uint16_t revId    = 0;        // REV_ID[31:16]
  uint16_t flashKb  = 0;        // размер флеша в КБ (0x1FFFF7E0)
  uint16_t rdpOpt   = 0;        // option byte RDP (0x1FFFF800)
  uint32_t pageSize = 1024;     // размер страницы стирания
  const char *density = "?";    // семейство/плотность
  const char *rdpText = "?";    // состояние read-out protection
};

// --- информация о цели ---
bool f1Probe(Swd &swd, Stm32F1Info &info);

// --- управление ядром ---
bool f1Halt(Swd &swd);
bool f1Run(Swd &swd);
bool f1SystemReset(Swd &swd);
bool f1ResetAndRun(Swd &swd);

// --- флеш-контроллер ---
bool f1EnsureHsi(Swd &swd);
bool f1Unlock(Swd &swd);
bool f1Lock(Swd &swd);
bool f1WaitReady(Swd &swd, uint32_t timeoutMs, uint32_t *sr);
bool f1ClearFlags(Swd &swd, uint32_t flags);
// Прочитать FLASH_SR, проверить/сбросить флаги ошибок (PGERR/WRPRTERR/EOP).
// Возвращает false, если были ошибки (значение SR остаётся в *srOut).
bool f1CheckErrors(Swd &swd, uint32_t *srOut);
bool f1ErasePage(Swd &swd, uint32_t pageAddr);
bool f1MassErase(Swd &swd);
bool f1ProgramEnable(Swd &swd, bool enable);
