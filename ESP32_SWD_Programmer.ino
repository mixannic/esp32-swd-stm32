// =============================================================================
//  ESP32_SWD_Programmer.ino
//
//  ESP32 в роли ST-Link-подобного программатора STM32F103 (SWD, 2 провода)
//  с веб-интерфейсом: загрузка .bin в ESP32 и прошивка STM32 по Wi-Fi.
//
//  Подключение (см. README.md):
//     ESP32 GPIO18 -> STM32 SWCLK (PA14, "CLK")
//     ESP32 GPIO23 -> STM32 SWDIO (PA13, "DIO")
//     ESP32 GND    -> STM32 GND
//     ESP32 3V3    -> STM32 3V3 (только если STM32 не питается от USB!)
// =============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "config.h"
#include "app_hooks.h"
#include "swd.h"
#include "stm32f1.h"
#include "web_ui.h"

#if ENABLE_MDNS
#include <ESPmDNS.h>
#endif

WebServer server(80);
Swd       swd;

// ------------------------- этапы задания ------------------------------------
enum JobStage : int {
  ST_OFF = 0, ST_CONNECT, ST_PROBE, ST_CHECK, ST_ERASE,
  ST_PROGRAM, ST_VERIFY, ST_RESET, ST_READ, ST_FSTAT, ST_DONE
};

static const char *stageName(int s)
{
  switch (s) {
    case ST_OFF:     return "простой";
    case ST_CONNECT: return "подключение к STM32";
    case ST_PROBE:   return "чтение информации о цели";
    case ST_CHECK:   return "проверка содержимого флеша";
    case ST_ERASE:   return "стирание страниц";
    case ST_PROGRAM: return "программирование";
    case ST_VERIFY:  return "верификация";
    case ST_RESET:   return "сброс цели";
    case ST_READ:    return "чтение флеша";
    case ST_FSTAT:   return "статус флеша";
    case ST_DONE:    return "завершено";
  }
  return "?";
}

// ------------------------- состояние ----------------------------------------
struct Job {
  volatile bool     busy       = false;
  volatile bool     cancel     = false;
  volatile int      stage      = ST_OFF;
  volatile uint32_t done       = 0;
  volatile uint32_t total      = 1;
  volatile bool     resultOk   = false;
  volatile bool     showResult = false;
  char msg[160] = "";
};
static Job g_job;

struct JobParams {
  bool     verify     = true;
  bool     skipMatch  = true;
  bool     resetAfter = true;
  uint8_t  speed      = SWD_SPEED_DEFAULT;
  uint32_t fwSize     = 0;
};
static JobParams g_p;

static volatile int  g_pendingJob = 0;      // 0 нет, 1 connect, 2 erase, 3 flash, 4 flashstat
static Stm32F1Info   g_target;
static volatile bool g_targetValid = false;

// Метаданные загруженной прошивки
static volatile bool g_fwExists = false;
static volatile uint32_t g_fwSize = 0;
static volatile uint32_t g_fwCrc  = 0;
static char g_fwName[40] = "";

// Статус флеша STM32 (задание ST_FSTAT, POST /api/flashstat): всё в байтах.
// valid=false -- данных ещё нет либо они устарели (после прошивки).
struct FlashStat {
  volatile bool     valid = false;
  volatile uint32_t total = 0;      // всего во флеше цели
  volatile uint32_t used  = 0;      // занято (по последнему байту != 0xFF)
  volatile uint32_t free  = 0;      // доступно = total - used
};
static FlashStat g_fs;


// ------------------------- лог ----------------------------------------------
static String g_log;
static SemaphoreHandle_t g_logMutex = nullptr;

void appLog(const char *fmt, ...)
{
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Serial.print(buf);

  if (!g_logMutex) return;
  if (xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    g_log += buf;
    if (g_log.length() > APP_LOG_SIZE) {
      g_log.remove(0, g_log.length() - APP_LOG_SIZE);
    }
    xSemaphoreGive(g_logMutex);
  }
}

void appProgress(uint32_t done, uint32_t total)
{
  g_job.done  = done;
  g_job.total = total ? total : 1;
}

bool appCancelRequested(void)
{
  return g_job.cancel;
}

static void jobFail(const char *msg)
{
  snprintf(g_job.msg, sizeof(g_job.msg), "%s", msg);
  appLog("ОШИБКА: %s\n", msg);
}

// ------------------------- утилиты ------------------------------------------
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

// Читает кусок прошивки из LittleFS в буфер, "хвост" заполняет 0xFF
// (стёртая ячейка флеша), возвращает число прочитанных байт из файла.
static uint32_t fwReadPage(File &f, uint32_t offset, uint8_t *dst, uint32_t pageSize, uint32_t fwSize)
{
  memset(dst, 0xFF, pageSize);
  if (offset >= fwSize) return 0;
  uint32_t len = fwSize - offset;
  if (len > pageSize) len = pageSize;
  if (!f.seek(offset)) return 0;
  return (uint32_t)f.read(dst, len);
}

static void jsonEscape(String &out, const String &in)
{
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04X", (unsigned)(uint8_t)c);
          out += b;
        } else {
          out += c;
        }
    }
  }
}

static void jobFailSwd(const char *what)
{
  char m[200];
  snprintf(m, sizeof(m), "%s: %s", what, swd.errorText());
  jobFail(m);
}

// Прогресс внутри "слайса" 0..1000: from..to -- полоса для фазы
static void progressRange(uint32_t from, uint32_t to, uint32_t cur, uint32_t total)
{
  if (total == 0) { appProgress(to, 1000); return; }
  if (cur > total) cur = total;
  appProgress(from + (uint32_t)((uint64_t)(to - from) * cur / total), 1000);
}

// ---------------------------------------------------------------------------
//  Общая часть всех заданий: подключение + чтение информации о цели
// ---------------------------------------------------------------------------
static bool jobConnect(void)
{
  g_job.stage = ST_CONNECT;
  swd.setSpeed(g_p.speed);
  swd.begin();

  if (!swd.connect()) {
    jobFail("нет связи по SWD (проверьте пины, GND, питание 3.3 В и скорость)");
    return false;
  }

  g_job.stage = ST_PROBE;
  Stm32F1Info info;
  if (!f1Probe(swd, info)) {
    jobFailSwd("не удалось прочитать параметры цели");
    return false;
  }
  g_target      = info;
  g_targetValid = true;
  return true;
}

// ---------------------------------------------------------------------------
//  Задание: массовое стирание
// ---------------------------------------------------------------------------
static bool jobErase(void)
{
  if (!jobConnect()) return false;

  if (!f1Halt(swd))    { jobFail("не удалось остановить ядро STM32"); return false; }
  if (!f1EnsureHsi(swd)) { jobFail("не удалось включить HSI"); return false; }

  g_job.stage = ST_ERASE;
  progressRange(0, 900, 0, 1);

  if (!f1MassErase(swd)) { jobFailSwd("массовое стирание не удалось"); return false; }
  progressRange(0, 900, 1, 1);

  f1Lock(swd);
  g_job.stage = ST_RESET;
  if (g_p.resetAfter) f1ResetAndRun(swd); else f1Run(swd);
  progressRange(900, 1000, 1, 1);
  // Массовое стирание гарантированно заполняет флеш 0xFF -> статус "пусто"
  if (g_fs.valid) { g_fs.used = 0; g_fs.free = g_fs.total; }
  appLog("Массовое стирание завершено\n");
  return true;
}

// ---------------------------------------------------------------------------
//  Задание: прошивка .bin в флеш STM32
//
//  Порядок (как у ST-Link/OpenOCD):
//    подключение -> останов ядра -> HSI -> unlock -> сравнение с текущим
//    содержимым -> стирание нужных страниц -> запись полусловами ->
//    верификация -> lock -> сброс и запуск
// ---------------------------------------------------------------------------
static bool jobFlash(void)
{
  const uint32_t addr0 = FLASH_LOAD_ADDR;
  const uint32_t fwSize = g_p.fwSize;

  if (fwSize == 0) {
    jobFail("файл прошивки не загружен -- сначала выберите .bin в браузере");
    return false;
  }

  if (!jobConnect()) return false;

  const uint32_t flashBytes = (uint32_t)g_target.flashKb * 1024UL;
  if (g_target.flashKb == 0 || fwSize > flashBytes) {
    char m[160];
    snprintf(m, sizeof(m), "прошивка (%u байт) не помещается во флеш цели (%u КБ)",
             (unsigned)fwSize, (unsigned)g_target.flashKb);
    jobFail(m);
    return false;
  }

  File f = LittleFS.open(FIRMWARE_PATH, FILE_READ);
  if (!f) {
    jobFail("не удалось открыть файл прошивки в LittleFS");
    return false;
  }

  if (!f1Halt(swd))      { f.close(); jobFail("не удалось остановить ядро STM32"); return false; }
  if (!f1EnsureHsi(swd)) { f.close(); jobFail("HSI не готов (нужен для записи флеша)"); return false; }
  if (!f1Unlock(swd))    { f.close(); jobFail("не удалось разблокировать флеш-контроллер"); return false; }

  const uint32_t pageSize = g_target.pageSize;
  const uint32_t pages    = (fwSize + pageSize - 1) / pageSize;

  bool *dirty = (bool *)malloc(pages ? pages : 1);
  if (!dirty) { f.close(); jobFail("нет памяти под карту страниц"); return false; }

  static uint8_t  buf[2048];    // страница целиком (максимум 2 КБ)
  static uint32_t rd[512];      // буфер чтения флеша (страница словами)

  // ---- ФАЗА 1: сравнение прошивки с содержимым флеша ---------------------
  g_job.stage = ST_CHECK;
  uint32_t dirtyCount = 0;
  for (uint32_t p = 0; p < pages; p++) {
    uint32_t off = p * pageSize;
    fwReadPage(f, off, buf, pageSize, fwSize);

    bool same = false;
    if (g_p.skipMatch) {
      if (!swd.memReadWords(addr0 + off, rd, pageSize / 4)) {
        free(dirty); f.close();
        jobFailSwd("ошибка чтения флеша при сравнении");
        return false;
      }
      same = (memcmp(rd, buf, pageSize) == 0);
    }
    dirty[p] = !same;
    if (!same) dirtyCount++;

    progressRange(0, 200, p + 1, pages);
    vTaskDelay(1);
  }
  appLog("Страниц: %u, требуют перезаписи: %u\n", (unsigned)pages, (unsigned)dirtyCount);
  if (dirtyCount == 0) {
    appLog("Флеш уже содержит ровно эту прошивку -- запись не требуется\n");
  }

  // ---- ФАЗА 2: стирание нужных страниц -----------------------------------
  g_job.stage = ST_ERASE;
  // Содержимое флеша сейчас изменится (даже если позже задание упадёт) --
  // прежний статус больше не действителен. При dirtyCount==0 ничего не меняется.
  if (dirtyCount) g_fs.valid = false;
  uint32_t erased = 0;
  for (uint32_t p = 0; p < pages; p++) {
    if (!dirty[p]) continue;
    if (!f1ErasePage(swd, addr0 + p * pageSize)) {
      free(dirty); f.close();
      jobFailSwd("ошибка стирания страницы");
      return false;
    }
    erased++;
    progressRange(200, 350, erased, dirtyCount);
    vTaskDelay(1);
  }
  appLog("Стёрто страниц: %u\n", (unsigned)erased);

  // ---- ФАЗА 3: программирование полусловами ------------------------------
  g_job.stage = ST_PROGRAM;
  uint32_t written = 0;
  for (uint32_t p = 0; p < pages; p++) {
    if (!dirty[p]) continue;

    uint32_t off = p * pageSize;
    fwReadPage(f, off, buf, pageSize, fwSize);

    // Ждём завершения предыдущей операции: запись в FLASH_CR при BSY=1
    // флеш-контроллер может проигнорировать.
    uint32_t sr = 0;
    if (!f1WaitReady(swd, 3000, &sr)) {
      free(dirty); f.close();
      jobFailSwd("флеш не готов к записи (таймаут BSY)");
      return false;
    }

    if (!f1ProgramEnable(swd, true)) {
      free(dirty); f.close();
      jobFailSwd("не удалось установить бит PG");
      return false;
    }

    // Страницу пишем кусками по 512 байт (256 полуслов): так обновляется
    // прогресс и Wi-Fi не блокируется надолго.
    for (uint32_t o = 0; o < pageSize; o += 512) {
      uint32_t n = pageSize - o;
      if (n > 512) n = 512;
      const uint16_t *hw = (const uint16_t *)(buf + o);   // выравнивание сохранено
      if (!swd.memWriteHalfwords(addr0 + off + o, hw, n / 2)) {
        free(dirty); f.close();
        jobFailSwd("ошибка записи во флеш");
        return false;
      }
      progressRange(350, 750, written * pageSize + o + n, dirtyCount * pageSize);
      vTaskDelay(1);
    }

    // Дожидаемся окончания программирования последнего полуслова страницы
    if (!f1WaitReady(swd, 3000, &sr)) {
      free(dirty); f.close();
      jobFailSwd("таймаут программирования страницы");
      return false;
    }

    if (!f1ProgramEnable(swd, false)) {
      free(dirty); f.close();
      jobFailSwd("не удалось снять бит PG");
      return false;
    }

    if (!f1CheckErrors(swd, &sr)) {
      char m[160];
      snprintf(m, sizeof(m), "ошибка программирования страницы 0x%08X (FLASH_SR=0x%08X)",
               (unsigned)(addr0 + off), (unsigned)sr);
      free(dirty); f.close();
      jobFail(m);
      return false;
    }

    // Быстрая проверка записи: первое слово страницы должно совпасть с файлом.
    // Ловит проблему сразу на первой странице вместо полной верификации в конце.
    {
      uint32_t got = 0, want = 0;
      memcpy(&want, buf, sizeof(want));
      if (!swd.memReadWords(addr0 + off, &got, 1) || got != want) {
        char m[160];
        snprintf(m, sizeof(m),
                 "запись страницы 0x%08X не подтверждена чтением "
                 "(прочитано 0x%08X, ожидалось 0x%08X)",
                 (unsigned)(addr0 + off), (unsigned)got, (unsigned)want);
        free(dirty); f.close();
        jobFail(m);
        return false;
      }
    }
    written++;
  }
  appLog("Записано страниц: %u\n", (unsigned)written);

  // ---- ФАЗА 4: верификация ------------------------------------------------
  if (g_p.verify) {
    g_job.stage = ST_VERIFY;
    uint32_t bad = 0, checked = 0;
    for (uint32_t p = 0; p < pages; p++) {
      if (!dirty[p]) continue;                   // совпадающие страницы уже сверены
      uint32_t off = p * pageSize;
      fwReadPage(f, off, buf, pageSize, fwSize);

      if (!swd.memReadWords(addr0 + off, rd, pageSize / 4)) {
        free(dirty); f.close();
        jobFailSwd("ошибка чтения флеша при верификации");
        return false;
      }
      if (memcmp(rd, buf, pageSize) != 0) {
        for (uint32_t i = 0; i < pageSize; i++) {
          uint8_t got = ((const uint8_t *)rd)[i];
          if (got != buf[i]) {
            appLog("Несовпадение @0x%08X: во флеше 0x%02X, в файле 0x%02X\n",
                   (unsigned)(addr0 + off + i), got, buf[i]);
            break;
          }
        }
        bad++;
      }
      checked++;
      progressRange(750, 950, checked, pages);
      vTaskDelay(1);
    }
    if (bad) {
      char m[128];
      snprintf(m, sizeof(m), "верификация не прошла (страниц с ошибками: %u)", (unsigned)bad);
      free(dirty); f.close();
      jobFail(m);
      return false;
    }
    appLog("Верификация прошла успешно\n");
  }
  free(dirty);

  // ---- ФАЗА 5: проверка вектора, lock, сброс ------------------------------
  uint32_t sp = 0, pc = 0;
  if (swd.memRead32(addr0, &sp) && swd.memRead32(addr0 + 4, &pc)) {
    appLog("Вектор сброса: SP=0x%08X, PC=0x%08X\n", (unsigned)sp, (unsigned)pc);
    if ((sp & 0xFFFF0000u) != 0x20000000u) {
      appLog("ВНИМАНИЕ: начальный SP не похож на ОЗУ (0x2000xxxx)\n");
    }
    if ((pc & 0xFF000000u) != 0x08000000u) {
      appLog("ВНИМАНИЕ: Reset_Handler не во флеше (0x0800xxxx)\n");
    }
  }

  if (!f1Lock(swd)) {
    appLog("предупреждение: не удалось закрыть флеш-контроллер\n");
  }

  g_job.stage = ST_RESET;
  if (g_p.resetAfter) {
    f1ResetAndRun(swd);
  } else {
    f1Run(swd);
  }
  progressRange(950, 1000, 1, 1);

  f.close();
  return true;
}

// ---------------------------------------------------------------------------
//  Задание: статус флеша -- занято / доступно / всего (в байтах).
//
//  "Занято" = смещение последнего байта, отличного от 0xFF (стираемое
//  состояние ячейки флеша), + 1. Чтение идёт блоками 1 КБ С КОНЦА флеша:
//  на пустой или частично заполненной цели работа прекращается сразу после
//  первого непустого блока.
// ---------------------------------------------------------------------------
static bool jobFlashStat(void)
{
  if (!jobConnect()) return false;

  const uint32_t total = (uint32_t)g_target.flashKb * 1024UL;
  if (total == 0 || total > (1024UL * 1024UL)) {
    jobFail("неизвестен размер флеша цели");
    return false;
  }

  g_job.stage = ST_FSTAT;
  progressRange(0, 1000, 0, 1);

  if (!f1Halt(swd)) {
    jobFail("не удалось остановить ядро STM32");
    return false;
  }

  static uint32_t rdBuf[256];        // 1 КБ -- по одному блоку за чтение
  const uint32_t  block = sizeof(rdBuf);
  uint32_t        used  = 0;
  bool            found = false;
  uint32_t        addr  = FLASH_LOAD_ADDR + total;

  while (addr > FLASH_LOAD_ADDR) {
    uint32_t chunk = block;
    if (addr - chunk < FLASH_LOAD_ADDR) chunk = addr - FLASH_LOAD_ADDR;
    addr -= chunk;

    if (!swd.memReadWords(addr, rdBuf, chunk / 4)) {
      f1Run(swd);
      jobFailSwd("ошибка чтения флеша при расчёте статуса");
      return false;
    }

    // Самый старший по адресу байт блока, отличный от 0xFF
    const uint8_t *b = (const uint8_t *)rdBuf;
    for (uint32_t i = chunk; i > 0; i--) {
      if (b[i - 1] != 0xFF) {
        used  = (addr - FLASH_LOAD_ADDR) + i;   // смещение за последним непустым байтом
        found = true;
        break;
      }
    }
    if (found) break;

    progressRange(0, 1000, total - (addr - FLASH_LOAD_ADDR), total);
    vTaskDelay(1);
  }

  if (!found) used = 0;

  g_fs.total = total;
  g_fs.used  = used;
  g_fs.free  = total - used;
  g_fs.valid = true;

  appLog("Статус флеша: всего %u байт, занято %u байт, свободно %u байт\n",
         (unsigned)total, (unsigned)used, (unsigned)(total - used));

  f1Run(swd);
  progressRange(0, 1000, 1, 1);
  return true;
}

// ---------------------------------------------------------------------------
//  Задача, выполняющая задание (веб-сервер продолжает отвечать).
//  Приоритет такой же, как у loop(), плюс vTaskDelay() между блоками.
// ---------------------------------------------------------------------------
static void jobTask(void *arg)
{
  int mode = (int)(intptr_t)arg;

  g_job.cancel     = false;
  g_job.done       = 0;
  g_job.total      = 1000;
  g_job.resultOk   = false;
  g_job.showResult = false;
  g_job.msg[0]     = 0;

  bool ok = false;
  switch (mode) {
    case 1:  ok = jobConnect();    break;
    case 2:  ok = jobErase();      break;
    case 4:  ok = jobFlashStat();  break;
    default: ok = jobFlash();      break;
  }

  g_job.stage      = ST_DONE;
  g_job.resultOk   = ok;
  g_job.showResult = true;
  if (!ok && g_job.msg[0] == 0) {
    snprintf(g_job.msg, sizeof(g_job.msg), "%s", swd.errorText());
  }
  appLog(ok ? "=== ГОТОВО ===\n" : "=== ОШИБКА ===\n");

  g_job.busy = false;
  vTaskDelete(NULL);
}

// ===========================================================================
//  HTTP-обработчики
// ===========================================================================
static void handleRoot()
{
  server.send_P(200, "text/html", WEB_PAGE);
}

static void handleNotFound()
{
  server.send(404, "text/plain", "not found");
}

static void handleStatus()
{
  String j;
  j.reserve(APP_LOG_SIZE + 1200);

  j += "{\"busy\":";
  j += String((int)(g_job.busy ? 1 : 0));
  j += ",\"pct\":";
  j += String((int)((uint64_t)g_job.done * 100 / (g_job.total ? g_job.total : 1)));
  j += ",\"stage\":\"";
  j += stageName(g_job.stage);
  j += "\",\"result\":\"";
  j += (g_job.showResult ? (g_job.resultOk ? "ok" : "err") : "");
  j += "\",\"msg\":\"";
  jsonEscape(j, String(g_job.msg));
  j += "\",\"ip\":\"";
  j += WiFi.localIP().toString();
  j += "\",\"rssi\":";
  j += String((int)WiFi.RSSI());

  j += ",\"target\":{\"valid\":";
  j += String((int)(g_targetValid ? 1 : 0));
  if (g_targetValid) {
    char b[192];
    snprintf(b, sizeof(b),
             ",\"idcode\":\"%08X\",\"apidr\":\"%08X\",\"dev\":\"0x%03X\",\"rev\":\"%04X\","
             "\"flash\":%u,\"page\":%u",
             (unsigned)g_target.idcode, (unsigned)g_target.apIdr,
             (unsigned)g_target.devId, (unsigned)g_target.revId,
             (unsigned)g_target.flashKb, (unsigned)g_target.pageSize);
    j += b;
    j += ",\"density\":\"";
    j += g_target.density;
    j += "\",\"rdp\":\"";
    j += g_target.rdpText;
    j += "\"";
  }
  j += "}";

  char b2[192];
  snprintf(b2, sizeof(b2),
           ",\"fw\":{\"exists\":%d,\"size\":%u,\"crc\":\"%08X\",\"name\":\"",
           g_fwExists ? 1 : 0, (unsigned)g_fwSize, (unsigned)g_fwCrc);
  j += b2;
  jsonEscape(j, String(g_fwName));
  j += "\"}";

  // Статус флеша STM32 (байты); valid=0 -- ещё не рассчитан или устарел
  char b3[128];
  snprintf(b3, sizeof(b3),
           ",\"fstat\":{\"valid\":%d,\"total\":%u,\"used\":%u,\"free\":%u}",
           g_fs.valid ? 1 : 0, (unsigned)g_fs.total,
           (unsigned)g_fs.used, (unsigned)g_fs.free);
  j += b3;

  j += ",\"log\":\"";
  if (g_logMutex && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    jsonEscape(j, g_log);
    xSemaphoreGive(g_logMutex);
  }
  j += "\"}";

  server.send(200, "application/json", j);
}

// -------- загрузка .bin в LittleFS ------------------------------------------
static File     g_upFile;
static uint32_t g_upSize = 0;
static uint32_t g_upCrc  = 0;
static bool     g_upErr  = false;

static void handleUploadData()
{
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    g_upSize = 0;
    g_upCrc  = 0;
    g_upErr  = false;
    if (g_job.busy) {
      g_upErr = true;
      appLog("Загрузка отклонена: идёт операция\n");
      return;
    }
    strncpy(g_fwName, up.filename.c_str(), sizeof(g_fwName) - 1);
    g_fwName[sizeof(g_fwName) - 1] = 0;
    LittleFS.remove(FIRMWARE_PATH);
    g_upFile = LittleFS.open(FIRMWARE_PATH, FILE_WRITE);
    if (!g_upFile) {
      g_upErr = true;
      appLog("Не удалось создать файл прошивки в LittleFS\n");
    }
  } else if (up.status == UPLOAD_FILE_WRITE || up.status == UPLOAD_FILE_END) {
    if (g_upErr || !g_upFile || up.currentSize == 0) return;
    if (g_upSize + up.currentSize > MAX_FIRMWARE_SIZE) {
      g_upErr = true;
      appLog("Файл больше допустимого (%u байт)\n", (unsigned)MAX_FIRMWARE_SIZE);
      g_upFile.close();
      return;
    }
    if (g_upFile.write(up.buf, up.currentSize) != up.currentSize) {
      g_upErr = true;
      return;
    }
    g_upCrc = crc32Update(g_upCrc, up.buf, up.currentSize);
    g_upSize += up.currentSize;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    g_upErr = true;
    if (g_upFile) g_upFile.close();
  }
}

static void handleUploadDone()
{
  if (g_upFile) g_upFile.close();

  if (g_upErr || g_upSize == 0) {
    LittleFS.remove(FIRMWARE_PATH);
    g_fwExists = false;
    server.send(500, "application/json", "{\"error\":\"ошибка загрузки файла\"}");
    return;
  }

  g_fwExists = true;
  g_fwSize   = g_upSize;
  g_fwCrc    = g_upCrc;
  g_p.fwSize = g_upSize;

  char msg[128];
  snprintf(msg, sizeof(msg), "{\"ok\":1,\"size\":%u,\"crc\":\"%08X\"}",
           (unsigned)g_upSize, (unsigned)g_upCrc);
  appLog("Прошивка загружена в ESP32: %u байт, CRC32=0x%08X\n",
         (unsigned)g_upSize, (unsigned)g_upCrc);
  server.send(200, "application/json", msg);
}

// -------- запуск заданий ----------------------------------------------------
static void beginJob(int mode)
{
  if (g_job.busy) {
    server.send(409, "application/json", "{\"error\":\"занято\"}");
    return;
  }
  g_pendingJob = mode;
  server.send(200, "application/json", "{\"started\":1}");
}

static void handleStartConnect()
{
  g_p.speed      = SWD_SPEED_DEFAULT;
  g_p.resetAfter = true;
  beginJob(1);
}

static void handleStartErase()
{
  g_p.speed      = SWD_SPEED_DEFAULT;
  g_p.resetAfter = true;
  beginJob(2);
}

// Статус флеша цели: занято / доступно / всего (байты), этап ST_FSTAT
static void handleStartFlashStat()
{
  g_p.speed      = SWD_SPEED_DEFAULT;
  g_p.resetAfter = true;
  beginJob(4);
}

static void handleStartFlash()
{
  if (server.hasArg("verify")) g_p.verify     = (server.arg("verify") == "1");
  if (server.hasArg("skip"))   g_p.skipMatch  = (server.arg("skip") == "1");
  if (server.hasArg("reset"))  g_p.resetAfter = (server.arg("reset") == "1");

  uint8_t sp = SWD_SPEED_DEFAULT;
  if (server.hasArg("speed")) {
    long v = server.arg("speed").toInt();
    if (v >= 1 && v <= 250) sp = (uint8_t)v;
  }
  g_p.speed = sp;

  if (!g_fwExists || g_fwSize == 0) {
    server.send(400, "application/json",
                "{\"error\":\"сначала загрузите .bin в ESP32\"}");
    return;
  }
  g_p.fwSize = g_fwSize;
  beginJob(3);
}

static void handleCancel()
{
  g_job.cancel = true;
  appLog("Пользователь нажал ОТМЕНА\n");
  server.send(200, "application/json", "{\"cancel\":1}");
}

// ===========================================================================
//  Выгрузка содержимого флеша STM32 в файл .bin (GET /api/readflash)
//  Потоковая отдача по 1 КБ: буфер в RAM не нужен, размер ограничен
//  MAX_FIRMWARE_SIZE не требуется -- читается весь флеш цели (flashKb*1024).
// ===========================================================================
static void handleReadFlash(void)
{
  if (g_job.busy) {
    server.send(409, "application/json", "{\"error\":\"идёт операция, подождите\"}");
    return;
  }

  g_job.busy       = true;
  g_job.cancel     = false;
  g_job.stage      = ST_READ;
  g_job.done       = 0;
  g_job.total      = 1;
  g_job.showResult = false;

  // Свежее подключение + опрос цели (после перезагрузки ESP32 данных о цели нет)
  swd.setSpeed(SWD_SPEED_DEFAULT);
  swd.begin();

  Stm32F1Info info;
  bool ok = swd.connect() && f1Probe(swd, info);
  if (ok) {
    g_target      = info;
    g_targetValid = true;
  }

  const uint32_t flashBytes = ok ? (uint32_t)info.flashKb * 1024UL : 0;
  if (!ok || flashBytes == 0 || flashBytes > (1024UL * 1024UL)) {
    g_job.stage  = ST_OFF;
    g_job.busy   = false;
    appLog("Чтение флеша: не удалось получить параметры цели\n");
    server.send(500, "application/json", "{\"error\":\"нет связи по SWD или неизвестен размер флеша\"}");
    return;
  }

  // Останавливаем ядро на время чтения (стабильнее), после -- запускаем обратно
  bool wasHalted = f1Halt(swd);

  char fname[48];
  snprintf(fname, sizeof(fname), "stm32_flash_%ukb.bin", (unsigned)info.flashKb);

  appLog("Чтение флеша: %u КБ, адрес 0x%08X...\n",
         (unsigned)info.flashKb, (unsigned)FLASH_LOAD_ADDR);

  server.sendHeader("Content-Disposition",
                    String("attachment; filename=\"") + fname + "\"");
  server.setContentLength(flashBytes);
  server.send(200, "application/octet-stream", "");

  static uint32_t rdBuf[256];        // 1 КБ -- по одному блоку за чтение
  const uint32_t  block = sizeof(rdBuf);
  uint32_t        addr  = FLASH_LOAD_ADDR;
  bool            err   = false;

  while (addr < FLASH_LOAD_ADDR + flashBytes) {
    if (!swd.memReadWords(addr, rdBuf, block / 4)) {
      appLog("Чтение флеша: ошибка на 0x%08X (%s)\n",
             (unsigned)addr, swd.errorText());
      err = true;
      break;
    }
    server.sendContent((const char *)rdBuf, block);
    addr += block;
    g_job.done  = addr - FLASH_LOAD_ADDR;
    g_job.total = flashBytes;
    if (appCancelRequested()) {
      appLog("Чтение флеша отменено\n");
      err = true;
      break;
    }
  }

  if (!err) {
    appLog("Чтение флеша завершено: %u КБ (файл %s)\n",
           (unsigned)info.flashKb, fname);
  }

  if (wasHalted) f1Run(swd);

  g_job.stage  = ST_OFF;
  g_job.busy   = false;
  g_job.cancel = false;
}

// ===========================================================================
//  Установка / основной цикл
// ===========================================================================
static void refreshFwInfo()
{
  File f = LittleFS.open(FIRMWARE_PATH, FILE_READ);
  if (!f) {
    g_fwExists = false;
    g_fwSize   = 0;
    g_fwCrc    = 0;
    return;
  }
  static uint8_t b[512];
  uint32_t sz  = f.size();
  uint32_t crc = 0;
  while (f.available()) {
    int n = f.read(b, sizeof(b));
    if (n <= 0) break;
    crc = crc32Update(crc, b, (size_t)n);
  }
  f.close();

  g_fwExists = (sz > 0);
  g_fwSize   = sz;
  g_fwCrc    = crc;
  g_p.fwSize = sz;
  if (sz > 0) {
    appLog("В памяти ESP32 уже есть прошивка: %u байт, CRC32=0x%08X\n",
           (unsigned)sz, (unsigned)crc);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32 SWD programmer for STM32F103 ===");
  Serial.printf("SWCLK = GPIO%d, SWDIO = GPIO%d\n", PIN_SWCLK, PIN_SWDIO);

  g_logMutex = xSemaphoreCreateMutex();

  if (!LittleFS.begin(true)) {
    appLog("LittleFS не смонтирован (попробуйте перезагрузить ESP32)\n");
  } else {
    appLog("LittleFS OK (%u КБ доступно)\n", (unsigned)(LittleFS.totalBytes() / 1024));
    refreshFwInfo();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setSleep(false);                 // меньше "джиттера" при bit-bang SWD
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  appLog("Подключение к Wi-Fi \"%s\"...\n", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 30000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    appLog("Wi-Fi OK! Откройте в браузере: http://%s/\n", WiFi.localIP().toString().c_str());
    appLog("Сигнал: %d dBm, канал %d\n", (int)WiFi.RSSI(), (int)WiFi.channel());
  } else {
    appLog("НЕ удалось подключиться к Wi-Fi \"%s\" за 30 с (проверьте SSID/пароль/2.4 ГГц)\n",
           WIFI_SSID);
  }

#if ENABLE_MDNS
  if (MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    appLog("mDNS: http://%s.local/\n", WIFI_HOSTNAME);
  }
#endif

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/upload",  HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/api/connect", HTTP_POST, handleStartConnect);
  server.on("/api/erase",   HTTP_POST, handleStartErase);
  server.on("/api/flashstat", HTTP_POST, handleStartFlashStat);
  server.on("/api/flash",   HTTP_POST, handleStartFlash);
  server.on("/api/cancel",  HTTP_POST, handleCancel);
  server.on("/api/readflash", HTTP_GET, handleReadFlash);
  server.onNotFound(handleNotFound);
  server.begin();

  appLog("Веб-сервер запущен (порт 80). Готов к работе.\n");
}

void loop()
{
  server.handleClient();

  if (g_pendingJob && !g_job.busy) {
    int mode = g_pendingJob;
    g_pendingJob = 0;
    g_job.busy = true;

    BaseType_t r = xTaskCreatePinnedToCore(jobTask, "swdjob", 8192,
                                           (void *)(intptr_t)mode, 1, NULL, 1);
    if (r != pdPASS) {
      g_job.busy = false;
      appLog("Не удалось создать задачу для выполнения операции\n");
    }
  }

  delay(2);
}
