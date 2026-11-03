#include "sim_gen.h"
#include "frame_log.h"
#include "serial_link.h"
#include "cfg_table.h"
#include "log_tee.h"

#define SIMLOG(fmt, ...)                        \
  do                                            \
  {                                             \
    logTeePrintf("[sim] " fmt, ##__VA_ARGS__);  \
    Serial0.printf("[sim] " fmt, ##__VA_ARGS__); \
  } while (0)

static bool gOn = false;
static uint8_t gState = 0x00;   // 8 路开关当前状态位图

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

void sim::begin() { set(cfgFlag("sim.io")); }

void sim::set(bool on)
{
  if (gOn == on) return;
  gOn = on;
  SIMLOG("%s\n", on ? "on" : "off");
}

bool sim::on() { return gOn; }

void sim::poll()
{
  if (!gOn) return;
  static uint32_t last = 0;
  if (millis() - last < 1500) return;
  last = millis();

  // 每帧随机翻转 0~2 路开关，制造状态演进（更像真实开关动作）
  int flips = esp_random() % 3;
  for (int i = 0; i < flips; i++)
    gState ^= (uint8_t)(1 << (esp_random() % 8));

  uint8_t f[6] = {0x01, 0x03, 0x01, gState, 0, 0};
  uint16_t crc = crc16(f, 4);
  f[4] = (uint8_t)(crc & 0xFF);       // Modbus RTU：CRC 低字节在前
  f[5] = (uint8_t)(crc >> 8);

  slSendRaw(f, 6);                    // 手机收到透传字节（原始报文）
  flAddRx(f, 6);                      // 上屏 / 落盘 / 规则解码（模拟从总线收到）
}
