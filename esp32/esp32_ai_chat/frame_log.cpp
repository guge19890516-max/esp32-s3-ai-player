#include "frame_log.h"
#include "text_codec.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

std::vector<FrameRec> frames;
bool flHexMode = false;

static SemaphoreHandle_t flMtx;
static flTap_t flTap = nullptr;

void flSetTap(flTap_t fn) { flTap = fn; }

static String flTimeOf(uint32_t ms)
{
  uint32_t s = ms / 1000;
  uint32_t h = s / 3600;
  s %= 3600;
  uint32_t m = s / 60;
  s %= 60;
  char b[16];
  if (h) snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
  else snprintf(b, sizeof(b), "%02u:%02u", (unsigned)m, (unsigned)s);
  return String(b);
}

String flTime(uint32_t ms) { return flTimeOf(ms); }

String flHexOf(const uint8_t *d, int n)
{
  String s;
  if (n > 0) s.reserve(n * 3);
  char b[4];
  for (int i = 0; i < n; i++)
  {
    if (i) s += ' ';
    snprintf(b, sizeof(b), "%02X", d[i]);
    s += b;
  }
  return s;
}

// 报文文本视图：保留完整 UTF-8 序列（含 3/4 字节的中文），其余不可见字节记 '.'
static String flTextOfStr(const String &t)
{
  String s;
  if (t.length()) s.reserve(t.length());
  for (int i = 0; i < t.length(); i++)
  {
    uint8_t c = (uint8_t)t[i];
    if (c == '\r' || c == '\n' || c == '\t') s += ' ';
    else if (c >= 0x20 && c < 0x7F) s += (char)c;
    else if (c >= 0xC2 && c < 0xE0 && i + 1 < t.length() && (uint8_t)t[i + 1] >= 0x80 && (uint8_t)t[i + 1] < 0xC0)
    {
      s += (char)c;
      s += t[i + 1];
      i++;
    }
    else if (c >= 0xE0 && c < 0xF0 && i + 2 < t.length() &&
             (uint8_t)t[i + 1] >= 0x80 && (uint8_t)t[i + 1] < 0xC0 &&
             (uint8_t)t[i + 2] >= 0x80 && (uint8_t)t[i + 2] < 0xC0)
    {
      s += t[i];
      s += t[i + 1];
      s += t[i + 2];
      i += 2;
    }
    else if (c >= 0xF0 && c < 0xF8 && i + 3 < t.length() &&
             (uint8_t)t[i + 1] >= 0x80 && (uint8_t)t[i + 1] < 0xC0 &&
             (uint8_t)t[i + 2] >= 0x80 && (uint8_t)t[i + 2] < 0xC0 &&
             (uint8_t)t[i + 3] >= 0x80 && (uint8_t)t[i + 3] < 0xC0)
    {
      s += t[i];
      s += t[i + 1];
      s += t[i + 2];
      s += t[i + 3];
      i += 3;
    }
    else s += '.';
  }
  s.trim();
  if (s.length() > FL_TEXT_MAX) s = s.substring(0, FL_TEXT_MAX);
  return s;
}

String flTextOf(const uint8_t *d, int n)
{
  String t;
  if (n > 0) t.concat((const char *)d, n);
  return flTextOfStr(t);
}

void flBegin()
{
  frames.clear();
  frames.reserve(FL_MAX_FRAMES);
  flMtx = xSemaphoreCreateMutex();
}

static void flPush(bool rx, int len, const String &hex, const String &text, uint32_t ms, const uint8_t *raw)
{
  if (flMtx) xSemaphoreTake(flMtx, portMAX_DELAY);
  FrameRec r;
  r.rx = rx;
  r.ms = ms;
  r.len = len;
  r.hex = hex;
  r.text = text;
  frames.push_back(r);
  while ((int)frames.size() > FL_MAX_FRAMES) frames.erase(frames.begin());
  if (flMtx) xSemaphoreGive(flMtx);
  if (flTap && raw && len > 0) flTap(rx, ms, raw, len);
}

void flAddRx(const uint8_t *d, int n)
{
  if (n <= 0) return;
  // 原始字节留 hex，文本视图按「接收编码」解码成 UTF-8（编码=UTF-8 时原样）
  String dec = tcDecodeRx(d, n);
  flPush(true, n, flHexOf(d, n), flTextOfStr(dec), millis(), d);
}

void flAddRxText(const String &s)
{
  if (!s.length()) return;
  int n = s.length();
  flPush(true, n, flHexOf((const uint8_t *)s.c_str(), n), s, millis(), (const uint8_t *)s.c_str());
}

void flAddTx(const String &s)
{
  if (!s.length()) return;
  int n = s.length();
  flPush(false, n, flHexOf((const uint8_t *)s.c_str(), n), flTextOfStr(s), millis(), (const uint8_t *)s.c_str());
}

void flAddTxRaw(const uint8_t *d, int n)
{
  if (n <= 0) return;
  String dec = tcDecodeRx(d, n);
  flPush(false, n, flHexOf(d, n), flTextOfStr(dec), millis(), d);
}

// 文本发送：hex 记实际上链路的编码字节，text 记原始 UTF-8 输入（屏幕显示人话）
void flAddTxWire(const String &utf8Text, const uint8_t *wire, int n)
{
  if (n <= 0 && !utf8Text.length()) return;
  flPush(false, n, flHexOf(wire, n), flTextOfStr(utf8Text), millis(), n > 0 ? wire : nullptr);
}

void flClear()
{
  if (flMtx) xSemaphoreTake(flMtx, portMAX_DELAY);
  frames.clear();
  if (flMtx) xSemaphoreGive(flMtx);
}

void flToggleHex()
{
  flHexMode = !flHexMode;
}