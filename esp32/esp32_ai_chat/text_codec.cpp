#include "text_codec.h"
#include "cfg_table.h"
#include "gbk_table.h"

// ---- UTF-8 基元 ----

void tcUtf8Put(String &s, uint32_t cp)
{
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800)
  {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000)
  {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
  else
  {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

// 从 s[i] 解析一个码点；非法序列返回 '?' 并只前进 1 字节
static uint32_t utf8Next(const String &s, int &i)
{
  uint8_t c = (uint8_t)s[i];
  int n = s.length();
  if (c < 0x80) { i += 1; return c; }
  if ((c & 0xE0) == 0xC0 && i + 1 < n && ((uint8_t)s[i + 1] & 0xC0) == 0x80)
  {
    uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F);
    i += 2;
    return cp ? cp : '?';
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < n &&
      ((uint8_t)s[i + 1] & 0xC0) == 0x80 && ((uint8_t)s[i + 2] & 0xC0) == 0x80)
  {
    uint32_t cp = ((c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F);
    i += 3;
    return cp ? cp : '?';
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < n &&
      ((uint8_t)s[i + 1] & 0xC0) == 0x80 && ((uint8_t)s[i + 2] & 0xC0) == 0x80 && ((uint8_t)s[i + 3] & 0xC0) == 0x80)
  {
    uint32_t cp = ((c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) |
                  (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F);
    i += 4;
    return cp ? cp : '?';
  }
  i += 1;
  return '?';
}

// ---- GBK（含 GB2312）----

static uint16_t uni2gbkChar(uint32_t cp)
{
  if (cp < 0x80) return (uint16_t)cp;
  if (cp >= 0x10000) return 0;
  return uniToGbk((uint16_t)cp);
}

String tcUtf8ToGbk(const String &utf8)
{
  String out;
  out.reserve(utf8.length() + 8);
  int i = 0;
  while (i < utf8.length())
  {
    uint32_t cp = utf8Next(utf8, i);
    uint16_t g = uni2gbkChar(cp);
    if (!g) { out += '?'; continue; }
    if (g < 0x80) out += (char)g;
    else
    {
      out += (char)(g >> 8);
      out += (char)(g & 0xFF);
    }
  }
  return out;
}

static bool gbkTrail(uint8_t t) { return (t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFE); }

String tcGbkToUtf8(const uint8_t *d, int n)
{
  String out;
  out.reserve(n + 8);
  for (int i = 0; i < n; i++)
  {
    uint8_t c = d[i];
    if (c < 0x80) { out += (char)c; continue; }
    if (c >= 0x81 && c <= 0xFE && i + 1 < n && gbkTrail(d[i + 1]))
    {
      int idx = (c - 0x81) * 190 + (d[i + 1] < 0x7F ? d[i + 1] - 0x40 : d[i + 1] - 0x41);
      tcUtf8Put(out, gbkToUni((uint16_t)idx));
      i++;
    }
    else out += '?';
  }
  return out;
}

// ---- UNICODE = UTF-16LE ----

String tcUtf8ToUni(const String &utf8, bool bom)
{
  String out;
  out.reserve(utf8.length() * 2 + 8);
  if (bom) { out += (char)0xFF; out += (char)0xFE; }
  int i = 0;
  while (i < utf8.length())
  {
    uint32_t cp = utf8Next(utf8, i);
    if (cp >= 0x10000)   // 代理对
    {
      uint32_t v = cp - 0x10000;
      uint16_t hi = (uint16_t)(0xD800 | (v >> 10));
      uint16_t lo = (uint16_t)(0xDC00 | (v & 0x3FF));
      out += (char)(hi & 0xFF); out += (char)(hi >> 8);
      out += (char)(lo & 0xFF); out += (char)(lo >> 8);
    }
    else
    {
      out += (char)(cp & 0xFF);
      out += (char)(cp >> 8);
    }
  }
  return out;
}

String tcUniToUtf8(const uint8_t *d, int n)
{
  String out;
  out.reserve(n + 8);
  int i = 0;
  if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) i = 2;   // 跳过 BOM
  while (i + 1 < n)
  {
    uint32_t cp = d[i] | ((uint32_t)d[i + 1] << 8);
    i += 2;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n)   // 高代理
    {
      uint32_t lo = d[i] | ((uint32_t)d[i + 1] << 8);
      if (lo >= 0xDC00 && lo <= 0xDFFF)
      {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
        tcUtf8Put(out, cp);
        continue;
      }
    }
    tcUtf8Put(out, cp);
  }
  return out;
}

// ---- 按配置入口 ----

String tcEncodeTx(const String &utf8)
{
  String enc = cfgTxEnc();
  if (enc == "GBK" || enc == "GB2312") return tcUtf8ToGbk(utf8);
  if (enc == "UNICODE") return tcUtf8ToUni(utf8, cfgUniBom());
  if (enc == "ASCII")
  {
    String out;
    out.reserve(utf8.length());
    int i = 0;
    while (i < utf8.length())
    {
      uint32_t cp = utf8Next(utf8, i);
      out += (cp < 0x80) ? (char)cp : '?';
    }
    return out;
  }
  return utf8;   // UTF-8 直通
}

String tcDecodeRx(const uint8_t *d, int n)
{
  if (n <= 0) return String();
  String enc = cfgRxEnc();
  if (enc == "GBK" || enc == "GB2312") return tcGbkToUtf8(d, n);
  if (enc == "UNICODE") return tcUniToUtf8(d, n);
  if (enc == "ASCII")
  {
    String out;
    out.reserve(n);
    for (int i = 0; i < n; i++) out += (d[i] < 0x80) ? (char)d[i] : '?';
    return out;
  }
  String out;   // UTF-8 直通
  out.concat((const char *)d, n);
  return out;
}
