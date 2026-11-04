#include "sim_gen.h"
#include "frame_log.h"
#include "serial_link.h"
#include "cfg_table.h"
#include "log_tee.h"

#define SIMLOG(fmt, ...)                         \
  do                                             \
  {                                              \
    logTeePrintf("[sim] " fmt, ##__VA_ARGS__);   \
    Serial0.printf("[sim] " fmt, ##__VA_ARGS__); \
  } while (0)

static bool gOn = false;        // 开关量帧
static bool gTe = false;        // 遥测帧
static uint64_t gState = 0x01;  // 开关量当前状态位图（bit0=路1），随机模式在此之上翻转

// Modbus RTU CRC16（多项式 0xA001，初值 0xFFFF）
static uint16_t crc16(const uint8_t *d, int n)
{
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < n; i++)
  {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// 读配置项当前值（发帧时读，配置页改动即时生效；1.5s 一次 NVS 读开销可忽略）
static String cfgVal(const char *k)
{
  int i = cfgIndexOf(k);
  return i >= 0 ? cfgGetRaw(cfgAt(i)) : String();
}

// hex 位图字符串 → 位图值（高字节在前，"01"=路1开，"8001"=路16+路1开）
static uint64_t hexBits(const String &s)
{
  uint64_t v = 0;
  for (int i = 0; i < s.length(); i++)
  {
    int c = s[i], d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c == ' ') continue;
    if (d < 0) continue;   // 非法字符跳过
    v = (v << 4) | (uint64_t)d;
  }
  return v;
}

// cnt 路范围内的有效位掩码
static uint64_t bitsMask(int cnt) { return cnt >= 64 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << cnt) - 1); }

void sim::begin()
{
  gOn = cfgFlag("sim.io");
  gTe = cfgFlag("sim.te");
  gState = hexBits(cfgVal("sim.mask"));   // 位图从配置恢复（按钮页改过会写回）
}

void sim::set(bool on)
{
  if (gOn == on) return;
  gOn = on;
  SIMLOG("io %s\n", on ? "on" : "off");
}

void sim::setTe(bool on)
{
  if (gTe == on) return;
  gTe = on;
  SIMLOG("te %s\n", on ? "on" : "off");
}

bool sim::on() { return gOn; }
bool sim::teOn() { return gTe; }

uint64_t sim::bits() { return gState; }

void sim::setBits(uint64_t v) { gState = v; }

// 开关量帧：01 03 <byteCount> <位图(大端)> CRC_LO CRC_HI
static void sendIoFrame()
{
  int cnt = cfgVal("sim.cnt").toInt();
  if (cnt < 1) cnt = 1;
  if (cnt > 64) cnt = 64;

  // 随机模式在当前位图上随机翻转 0~2 路（贴近真实开关动作，瞬态不写回配置）；
  // 固定模式直接发当前位图（屏幕按钮页编辑的就是它）
  uint64_t st = gState & bitsMask(cnt);
  if (cfgVal("sim.mode") != "固定")
  {
    int flips = esp_random() % 3;
    for (int i = 0; i < flips; i++)
      st ^= 1ULL << (esp_random() % cnt);
  }
  gState = st;

  int nb = (cnt + 7) / 8;
  uint8_t f[3 + 8 + 2];
  f[0] = 0x01;
  f[1] = 0x03;
  f[2] = (uint8_t)nb;
  for (int i = 0; i < nb; i++)
    f[3 + i] = (uint8_t)((st >> (8 * (nb - 1 - i))) & 0xFF);   // 位图大端
  uint16_t crc = crc16(f, 3 + nb);
  f[3 + nb] = (uint8_t)(crc & 0xFF);
  f[4 + nb] = (uint8_t)(crc >> 8);
  int len = 5 + nb;

  slSendRaw(f, len);
  flAddRx(f, len);
}

// 遥测帧：01 04 <2N> <通道值u16BE...> CRC_LO CRC_HI
// 通道数/基准值可配（te.cnt 1~8 / te.ch1~te.ch8），波动 = 基准 ±1%（至少 ±1）
static void sendTeFrame()
{
  int n = cfgVal("te.cnt").toInt();
  if (n < 1) n = 1;
  if (n > 8) n = 8;

  uint8_t f[3 + 16 + 2];
  f[0] = 0x01;
  f[1] = 0x04;
  f[2] = (uint8_t)(2 * n);
  for (int i = 0; i < n; i++)
  {
    char k[8];
    snprintf(k, sizeof(k), "te.ch%d", i + 1);
    long base = cfgVal(k).toInt();
    if (base < 0) base = 0;
    if (base > 65535) base = 65535;
    long jit = base / 100;
    if (jit < 1) jit = 1;
    long v = base + (long)(esp_random() % (2 * jit + 1)) - jit;
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    f[3 + i * 2] = (uint8_t)(v >> 8);      // 大端
    f[4 + i * 2] = (uint8_t)(v & 0xFF);
  }
  int len = 3 + 2 * n;
  uint16_t crc = crc16(f, len);
  f[len] = (uint8_t)(crc & 0xFF);
  f[len + 1] = (uint8_t)(crc >> 8);
  len += 2;

  slSendRaw(f, len);
  flAddRx(f, len);
}

void sim::poll()
{
  static uint32_t lastIo = 0, lastTe = 0;
  uint32_t now = millis();
  if (gOn && now - lastIo >= 1500)
  {
    lastIo = now;
    sendIoFrame();
  }
  if (gTe && now - lastTe >= 1500)
  {
    lastTe = now;
    sendTeFrame();
  }
}
