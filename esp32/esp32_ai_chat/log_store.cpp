#include "log_store.h"
#include "board_config.h"
#include "cfg_table.h"
#include "log_tee.h"
#include <SD_MMC.h>
#include <time.h>
#include <vector>

static const uint32_t VOL_MAX = 2 * 1024 * 1024;  // 单文件 2MB 分卷
static const size_t FLUSH_AT = 4096;              // 队列攒到 4KB 就刷
static const size_t PEND_CAP = 32 * 1024;         // 队列上限（刷不出去就丢新帧，不涨内存）

static bool mounted = false;
static bool active = false;
static String sessName;      // log_1103_1432
static String sessDir;       // /logs/log_1103_1432
static File framesFile;
static File csvFile;
static bool csvOpen = false;
static uint32_t fileBytes = 0;      // 当前分卷已写
static uint32_t sessionBytes = 0;   // 本会话 frames.jsonl 累计
static int volIdx = 1;
static std::vector<uint8_t> pend;
static uint32_t drops = 0;

bool lsMounted() { return mounted; }

bool lsEnsureMount()
{
  if (mounted) return true;
  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0)) return false;
  // 1-bit 模式（CLK/CMD/D0 三线），与 ota_update 的挂法一致
  if (!SD_MMC.begin("/sdcard", true, false, 20000, 4)) return false;
  if (SD_MMC.cardType() == CARD_NONE) { SD_MMC.end(); return false; }
  mounted = true;
  logTeePrintf("[sd] mounted\n");
  return true;
}

static String makeSessionName()
{
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year + 1900 >= 2024)
  {
    char b[24];
    strftime(b, sizeof(b), "log_%m%d_%H%M", &t);
    return String(b);
  }
  // 时间未同步：用运行秒数兜底，保证目录名可用
  char b[24];
  snprintf(b, sizeof(b), "log_u%lu", (unsigned long)(millis() / 1000));
  return String(b);
}

bool lsActive() { return active; }

String lsSession() { return active ? sessName : String(); }

String lsStatus()
{
  if (!active) return "未记录";
  char b[40];
  snprintf(b, sizeof(b), "%s  %lu KB", sessName.c_str(), (unsigned long)(sessionBytes / 1024));
  return String(b);
}

void lsBegin() {}

// 分卷：关当前 frames.jsonl，开 frames.<n>.jsonl
static void rollover()
{
  framesFile.close();
  volIdx++;
  char b[24];
  snprintf(b, sizeof(b), "/frames.%d.jsonl", volIdx);
  framesFile = SD_MMC.open(sessDir + b, FILE_WRITE);
  fileBytes = 0;
  logTeePrintf("[sd] rollover vol=%d\n", volIdx);
}

static void flushPend()
{
  if (!pend.size()) return;
  if (!active || !framesFile) { pend.clear(); return; }
  framesFile.write(pend.data(), pend.size());
  fileBytes += pend.size();
  sessionBytes += pend.size();
  pend.clear();
  if (fileBytes >= VOL_MAX) rollover();
}

bool lsStart(const char *reason)
{
  if (active) lsStop();
  if (!lsEnsureMount()) return false;

  String name = makeSessionName();
  String dir = "/logs/" + name;
  if (SD_MMC.exists(dir))
  {
    for (int i = 2; i < 10; i++)
    {
      String d2 = dir + "-" + String(i);
      if (!SD_MMC.exists(d2)) { dir = d2; break; }
    }
  }
  SD_MMC.mkdir("/logs");
  SD_MMC.mkdir(dir);

  framesFile = SD_MMC.open(dir + "/frames.jsonl", FILE_WRITE);
  if (!framesFile) return false;
  sessName = name;
  sessDir = dir;
  volIdx = 1;
  fileBytes = 0;
  sessionBytes = 0;
  drops = 0;
  pend.clear();
  csvOpen = false;   // decoded.csv 惰性打开（第一行解析结果来时再建文件）
  active = true;
  logTeePrintf("[sd] session start %s (%s)\n", dir.c_str(), reason ? reason : "manual");
  return true;
}

bool lsStop()
{
  if (!active) return false;
  flushPend();
  if (framesFile) framesFile.close();
  if (csvOpen) csvFile.close();
  csvOpen = false;
  active = false;
  logTeePrintf("[sd] session stop %s bytes=%lu drops=%lu\n",
               sessName.c_str(), (unsigned long)sessionBytes, (unsigned long)drops);
  return true;
}

void lsFrame(bool rx, uint32_t ms, const uint8_t *d, int n)
{
  if (!active || !d || n <= 0) return;
  String hex;
  hex.reserve(n * 3);
  char b[4];
  for (int i = 0; i < n; i++)
  {
    if (i) hex += ' ';
    snprintf(b, sizeof(b), "%02X", d[i]);
    hex += b;
  }
  String line = String("{\"t\":") + ms + ",\"rx\":" + (rx ? 1 : 0) + ",\"len\":" + n + ",\"hex\":\"" + hex + "\"}\n";
  if (pend.size() + line.length() > PEND_CAP) { drops++; return; }
  pend.insert(pend.end(), (const uint8_t *)line.c_str(), (const uint8_t *)line.c_str() + line.length());
}

void lsDecoded(uint32_t ms, int dev, const String &csvVals)
{
  if (!active || !cfgFlag("dec.csv")) return;
  if (!csvOpen)
  {
    csvFile = SD_MMC.open(sessDir + "/decoded.csv", FILE_WRITE);
    if (!csvFile) return;
    csvOpen = true;
  }
  String row = String(ms) + "," + dev + "," + csvVals + "\n";
  csvFile.print(row);
}

void lsPoll()
{
  if (!active) return;
  if (pend.size() >= FLUSH_AT) flushPend();
  else
  {
    // 低速时每 2 秒也刷一次，避免最后一帧长时间滞留内存
    static uint32_t lastFlush = 0;
    if (pend.size() && millis() - lastFlush > 2000)
    {
      lastFlush = millis();
      flushPend();
    }
  }
}
