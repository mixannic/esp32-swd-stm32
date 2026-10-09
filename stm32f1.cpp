// =============================================================================
//  stm32f1.cpp -- программирование флеша STM32F1 через MEM-AP (AHB-AP)
// =============================================================================
#include <Arduino.h>
#include "config.h"
#include "app_hooks.h"
#include "swd.h"
#include "stm32f1.h"

// ------------------------- адреса регистров --------------------------------
#define FLASH_REG_BASE     0x40022000UL
#define FLASH_KEYR         (FLASH_REG_BASE + 0x04)
#define FLASH_OPTKEYR      (FLASH_REG_BASE + 0x08)
#define FLASH_SR           (FLASH_REG_BASE + 0x0C)
#define FLASH_CR           (FLASH_REG_BASE + 0x10)
#define FLASH_AR           (FLASH_REG_BASE + 0x14)
#define FLASH_OBR          (FLASH_REG_BASE + 0x1C)
#define FLASH_WRPR         (FLASH_REG_BASE + 0x20)

#define FLASH_CR_PG        (1UL << 0)
#define FLASH_CR_PER       (1UL << 1)
#define FLASH_CR_MER       (1UL << 2)
#define FLASH_CR_OPTPG     (1UL << 4)
#define FLASH_CR_OPTER     (1UL << 5)
#define FLASH_CR_STRT      (1UL << 6)
#define FLASH_CR_LOCK      (1UL << 7)

#define FLASH_SR_BSY       (1UL << 0)
#define FLASH_SR_PGERR     (1UL << 2)
#define FLASH_SR_WRPRTERR  (1UL << 4)
#define FLASH_SR_EOP       (1UL << 5)

#define FLASH_KEY1         0x45670123UL
#define FLASH_KEY2         0xCDEF89ABUL

#define RCC_CR             0x40021000UL
#define RCC_CR_HSION       (1UL << 0)
#define RCC_CR_HSIRDY      (1UL << 1)

#define DBGMCU_IDCODE      0xE0042000UL
#define FLASH_SIZE_ADDR    0x1FFFF7E0UL
#define OPT_RDP_ADDR       0x1FFFF800UL

#define CORE_DHCSR         0xE000EDF0UL
#define CORE_AIRCR         0xE000ED0CUL

#define DHCSR_KEY          0xA05F0000UL
#define DHCSR_C_DEBUGEN    (1UL << 0)
#define DHCSR_C_HALT       (1UL << 1)
#define DHCSR_S_HALT       (1UL << 17)

#define AIRCR_VECTKEY      0x05FA0000UL
#define AIRCR_SYSRESETREQ  (1UL << 2)

// ------------------------- вспомогательные таблицы -------------------------
static const char *densityName(uint16_t devId)
{
  switch (devId) {
    case 0x412: return "low-density (x4/x6, 16-32 КБ)";
    case 0x410: return "medium-density (x8/xB, 64-128 КБ)";
    case 0x414: return "high-density (xC/xD/xE, 256-512 КБ)";
    case 0x418: return "connectivity line (STM32F105/107)";
    case 0x420: return "value line (STM32F100)";
    case 0x430: return "XL-density (768-1024 КБ)";
    default:    return "неизвестный DEV_ID";
  }
}

static const char *rdpName(uint16_t rdpOpt)
{
  // option byte = (инверсный байт << 8) | байт
  switch (rdpOpt) {
    case 0x5AA5: return "нет (уровень 0)";
    case 0xFF00:
    case 0x00FF: return "УРОВЕНЬ 1 (чтение флеша запрещено!)";
    case 0x33CC:
    case 0xCC33: return "УРОВЕНЬ 2 (необратимо!)";
    default:     return "неизвестно";
  }
}

// ---------------------------------------------------------------------------
//  Чтение информации о цели
// ---------------------------------------------------------------------------
bool f1Probe(Swd &swd, Stm32F1Info &info)
{
  info.valid  = false;
  info.idcode = swd.idcode();
  info.apIdr  = swd.apIdr();

  if (!swd.memRead32(DBGMCU_IDCODE, &info.dbgmcuId)) {
    appLog("STM32: не читается DBGMCU_IDCODE (0xE0042000)\n");
    return false;
  }
  info.devId = (uint16_t)(info.dbgmcuId & 0x0FFFu);
  info.revId = (uint16_t)((info.dbgmcuId >> 16) & 0xFFFFu);

  if (!swd.memRead16(FLASH_SIZE_ADDR, &info.flashKb)) {
    appLog("STM32: не читается регистр размера флеша\n");
    return false;
  }

  // Если регистр размера недоступен/испорчен -- берём типовой размер по DEV_ID
  if (info.flashKb == 0 || info.flashKb > 4096) {
    uint16_t guess = 64;
    switch (info.devId) {
      case 0x412: guess = 32;   break;   // low-density x4/x6
      case 0x410: guess = 128;  break;   // medium-density x8/xB
      case 0x414: guess = 512;  break;   // high-density
      case 0x418: guess = 256;  break;   // connectivity line
      case 0x420: guess = 128;  break;   // value line
      case 0x430: guess = 1024; break;   // XL-density
      default:    guess = 64;   break;
    }
    appLog("STM32: размер флеша прочитан как %u -- беру оценку по DEV_ID: %u КБ\n",
           (unsigned)info.flashKb, (unsigned)guess);
    info.flashKb = guess;
  }

  uint16_t rdp = 0;
  swd.memRead16(OPT_RDP_ADDR, &rdp);
  info.rdpOpt  = rdp;
  info.rdpText = rdpName(rdp);
  info.density = densityName(info.devId);

  // Размер страницы: у F1 low/medium-density (<=128 КБ) -- 1 КБ, дальше -- 2 КБ
  info.pageSize = (info.flashKb <= 128) ? 1024UL : 2048UL;
  info.valid    = true;

  appLog("STM32: DEV_ID=0x%03X rev=0x%04X (%s)\n", info.devId, info.revId, info.density);
  appLog("STM32: FLASH=%u КБ, страница=%u Б, RDP=%s\n",
         (unsigned)info.flashKb, (unsigned)info.pageSize, info.rdpText);

  if (info.devId != 0x412 && info.devId != 0x410 && info.devId != 0x414 &&
      info.devId != 0x418 && info.devId != 0x420 && info.devId != 0x430) {
    appLog("STM32: ВНИМАНИЕ: неизвестный DEV_ID -- возможно, это не STM32F1,\n"
           "        и алгоритм записи флеша для него не подойдёт!\n");
  }
  if (rdp != 0x5AA5) {
    appLog("STM32: ВНИМАНИЕ! Включена защита от чтения: стирание возможно,\n"
           "        а верификация (чтение флеша) -- нет. Снять защиту можно\n"
           "        ST-Link Utility / STM32CubeProgrammer (это сотрёт флеш).\n");
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Останов/запуск ядра (DHCSR): DBGKEY | C_DEBUGEN | C_HALT
// ---------------------------------------------------------------------------
bool f1Halt(Swd &swd)
{
  uint32_t dhcsr = DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT;
  if (!swd.memWrite32(CORE_DHCSR, dhcsr)) {
    appLog("STM32: не удалось записать DHCSR\n");
    return false;
  }
  for (int i = 0; i < 100; i++) {
    uint32_t v = 0;
    if (!swd.memRead32(CORE_DHCSR, &v)) {
      appLog("STM32: не удалось прочитать DHCSR при останове\n");
      return false;
    }
    if (v & DHCSR_S_HALT) {
      appLog("STM32: ядро остановлено\n");
      return true;
    }
    delay(10);
  }
  appLog("STM32: ядро не остановилось (нет бита S_HALT)\n");
  return false;
}

bool f1Run(Swd &swd)
{
  // C_HALT = 0 -> ядро продолжает выполнение
  return swd.memWrite32(CORE_DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN);
}

bool f1SystemReset(Swd &swd)
{
  // AIRCR: VECTKEY | SYSRESETREQ
  return swd.memWrite32(CORE_AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);
}

bool f1ResetAndRun(Swd &swd)
{
  if (!f1Run(swd)) {
    appLog("STM32: предупреждение: не удалось снять C_HALT\n");
  }
  if (!f1SystemReset(swd)) {
    appLog("STM32: предупреждение: не удалось выполнить системный сброс\n");
  }
  delay(100);

  uint32_t v = 0;
  if (swd.memRead32(CORE_DHCSR, &v)) {
    appLog("STM32: сброс выполнен, ядро запущено (DHCSR=0x%08X)\n", v);
    return true;
  }
  appLog("STM32: после сброса связь по SWD потеряна (прошивка отключила SWD?)\n");
  return false;
}

// ---------------------------------------------------------------------------
//  Часы для флеш-контроллера: HSI (внутренний RC 8 МГц) должен быть включён.
//  После сброса он включён, но приложение могло его выключить.
// ---------------------------------------------------------------------------
bool f1EnsureHsi(Swd &swd)
{
  uint32_t cr = 0;
  if (!swd.memRead32(RCC_CR, &cr)) return false;
  if ((cr & RCC_CR_HSION) == 0) {
    appLog("STM32: включаю HSI (нужен для программирования флеша)\n");
    cr |= RCC_CR_HSION;
    if (!swd.memWrite32(RCC_CR, cr)) return false;
  }
  for (int i = 0; i < 50; i++) {
    if (!swd.memRead32(RCC_CR, &cr)) return false;
    if (cr & RCC_CR_HSIRDY) return true;
    delay(2);
  }
  appLog("STM32: HSI не готов (HSIRDY=0)\n");
  return false;
}

// ---------------------------------------------------------------------------
//  Разблокировка/блокировка флеш-контроллера (ключи как в RM0008)
// ---------------------------------------------------------------------------
bool f1Unlock(Swd &swd)
{
  uint32_t cr = 0;
  if (!swd.memRead32(FLASH_CR, &cr)) return false;
  if ((cr & FLASH_CR_LOCK) == 0) return true;      // уже разблокирован

  if (!swd.memWrite32(FLASH_KEYR, FLASH_KEY1)) return false;
  if (!swd.memWrite32(FLASH_KEYR, FLASH_KEY2)) return false;

  if (!swd.memRead32(FLASH_CR, &cr)) return false;
  if (cr & FLASH_CR_LOCK) {
    appLog("STM32: не удалось разблокировать флеш-контроллер (LOCK=1).\n"
           "        Помогает сброс цели (или переподключение с кнопкой RESET).\n");
    return false;
  }
  appLog("STM32: флеш-контроллер разблокирован\n");
  return true;
}

bool f1Lock(Swd &swd)
{
  return swd.memWrite32(FLASH_CR, FLASH_CR_LOCK);
}

// ---------------------------------------------------------------------------
//  Ожидание окончания операции флеш-контроллера (бит BSY в FLASH_SR)
// ---------------------------------------------------------------------------
bool f1WaitReady(Swd &swd, uint32_t timeoutMs, uint32_t *sr)
{
  uint32_t t0 = millis();
  for (;;) {
    uint32_t v = 0;
    if (!swd.memRead32(FLASH_SR, &v)) return false;
    if (sr) *sr = v;
    if ((v & FLASH_SR_BSY) == 0) return true;

    if ((millis() - t0) > timeoutMs) {
      appLog("STM32: таймаут ожидания BSY (FLASH_SR=0x%08X)\n", v);
      return false;
    }
    if (appCancelRequested()) return false;
    delay(1);
  }
}

// Флаги FLASH_SR сбрасываются записью единиц (write-1-to-clear)
bool f1ClearFlags(Swd &swd, uint32_t flags)
{
  if (flags == 0) return true;
  return swd.memWrite32(FLASH_SR, flags);
}

// Прочитать FLASH_SR, проверить и сбросить флаги ошибок.
// Возвращает false, если были PGERR/WRPRTERR.
bool f1CheckErrors(Swd &swd, uint32_t *srOut)
{
  uint32_t sr = 0;
  if (!swd.memRead32(FLASH_SR, &sr)) return false;
  if (srOut) *srOut = sr;

  uint32_t errs = sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR);
  if (errs) {
    if (errs & FLASH_SR_WRPRTERR) {
      uint32_t wrpr = 0;
      swd.memRead32(FLASH_WRPR, &wrpr);
      appLog("STM32: WRPRTERR -- часть флеша защищена от записи (FLASH_WRPR=0x%08X)\n",
             (unsigned)wrpr);
    }
    if (errs & FLASH_SR_PGERR) {
      appLog("STM32: PGERR -- попытка программирования нестёртой ячейки\n");
    }
  }
  f1ClearFlags(swd, errs | (sr & FLASH_SR_EOP));
  return (errs == 0);
}

// ---------------------------------------------------------------------------
//  Стирание одной страницы: PER=1 -> FLASH_AR=адрес страницы -> STRT=1
// ---------------------------------------------------------------------------
bool f1ErasePage(Swd &swd, uint32_t pageAddr)
{
  uint32_t sr = 0;

  if (!f1WaitReady(swd, 2000, &sr)) return false;
  if (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
    f1ClearFlags(swd, FLASH_SR_PGERR | FLASH_SR_WRPRTERR);
  }

  if (!swd.memWrite32(FLASH_CR, FLASH_CR_PER)) return false;
  if (!swd.memWrite32(FLASH_AR, pageAddr)) return false;
  if (!swd.memWrite32(FLASH_CR, FLASH_CR_PER | FLASH_CR_STRT)) return false;

  bool ok = f1WaitReady(swd, 5000, &sr);
  swd.memWrite32(FLASH_CR, 0);                     // снять PER

  if (!ok) return false;
  if (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
    appLog("STM32: ошибка стирания страницы 0x%08X (SR=0x%08X)\n", pageAddr, sr);
    f1ClearFlags(swd, FLASH_SR_PGERR | FLASH_SR_WRPRTERR);
    return false;
  }
  f1ClearFlags(swd, FLASH_SR_EOP);
  return true;
}

// ---------------------------------------------------------------------------
//  Массовое стирание всего флеша: MER=1 -> MER|STRT=1
// ---------------------------------------------------------------------------
bool f1MassErase(Swd &swd)
{
  uint32_t sr = 0;

  if (!f1Unlock(swd)) return false;
  if (!f1WaitReady(swd, 2000, &sr)) return false;

  if (!swd.memWrite32(FLASH_CR, FLASH_CR_MER)) return false;
  if (!swd.memWrite32(FLASH_CR, FLASH_CR_MER | FLASH_CR_STRT)) return false;

  bool ok = f1WaitReady(swd, 30000, &sr);
  swd.memWrite32(FLASH_CR, 0);

  if (!ok) return false;
  if (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) {
    appLog("STM32: ошибка массового стирания (SR=0x%08X)\n", sr);
    f1ClearFlags(swd, FLASH_SR_PGERR | FLASH_SR_WRPRTERR);
    return false;
  }
  f1ClearFlags(swd, FLASH_SR_EOP);
  appLog("STM32: массовое стирание выполнено\n");
  return true;
}

// ---------------------------------------------------------------------------
//  Включение/выключение режима программирования (бит PG в FLASH_CR)
// ---------------------------------------------------------------------------
bool f1ProgramEnable(Swd &swd, bool enable)
{
  return swd.memWrite32(FLASH_CR, enable ? FLASH_CR_PG : 0);
}
