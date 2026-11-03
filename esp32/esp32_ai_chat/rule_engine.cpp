#include "rule_engine.h"
#include "serial_link.h"
#include "log_store.h"
#include "log_tee.h"
#include <ArduinoJson.h>
#include <SD_MMC.h>

using namespace re;   // 本文件全是规则引擎实现，直接用 re:: 下的类型与枚举

#define RELOG(fmt, ...)                     \
  do                                        \
  {                                         \
    logTeePrintf("[rule] " fmt, ##__VA_ARGS__); \
    Serial0.printf("[rule] " fmt, ##__VA_ARGS__); \
  } while (0)

static Rule gRule;
static bool gHas = false;
static bool gPush = false;

// —— 每设备当前值（有 addr 时按地址分组；单设备地址固定 0）——
struct DevState { int addr; uint32_t ms; std::vector<String> vals; };
static std::vector<DevState> gDevs;
static const int DEV_MAX = 16;

static const char *TYPENAMES[] = {"u8", "i8", "u16", "i16", "u32", "i32", "f32", "bit"};
static const uint8_t TYPESIZE[] = {1, 1, 2, 2, 4, 4, 4, 1};

static uint8_t typeNameToEnum(const char *s, bool forAddr)
{
  if (!strcmp(s, "u8")) return FT_U8;
  if (!strcmp(s, "i8")) return FT_I8;
  if (forAddr) return 0xFF;   // addr 只允许 u8/u16
  if (!strcmp(s, "u16")) return FT_U16;
  if (!strcmp(s, "i16")) return FT_I16;
  if (!strcmp(s, "u32")) return FT_U32;
  if (!strcmp(s, "i32")) return FT_I32;
  if (!strcmp(s, "f32")) return FT_F32;
  if (!strcmp(s, "bit")) return FT_BIT;
  return 0xFF;
}

void re::setPush(bool on) { gPush = on; }
bool re::pushOn() { return gPush; }
bool re::hasRule() { return gHas; }
String re::ruleName() { return gHas ? gRule.name : String(); }

// —— 规则名 → 安全文件名：去路径分隔/控制字符，限 48 字节 ——
static String safeName(const String &n)
{
  String s;
  s.reserve(min((int)n.length(), 48));
  for (int i = 0; i < (int)n.length() && (int)s.length() < 48; i++)
  {
    char c = n[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|' || c < 0x20)
      continue;
    s += c;
  }
  s.trim();
  return s;
}

// —— 规则 JSON 序列化（rule.get 读回 / 存盘用同一份）——
static void serializeRule(const Rule &r, String &out)
{
  JsonDocument d;
  d["name"] = r.name;
  d["ver"] = r.ver;
  if (r.src.length()) d["src"] = r.src;
  if (r.multi)
  {
    JsonObject a = d["addr"].to<JsonObject>();
    a["off"] = r.addrOff;
    a["type"] = (r.addrType == FT_U16) ? "u16" : "u8";
    if (r.addrBe) a["be"] = 1;
  }
  if (r.match.size())
  {
    JsonArray m = d["match"].to<JsonArray>();
    for (auto &mm : r.match)
    {
      JsonObject e = m.add<JsonObject>();
      e["off"] = mm.off;
      JsonArray vals = e["val"].to<JsonArray>();
      for (auto v : mm.vals) vals.add(v);
    }
  }
  JsonArray fs = d["fields"].to<JsonArray>();
  for (auto &f : r.fields)
  {
    JsonObject e = fs.add<JsonObject>();
    e["name"] = f.name;
    e["off"] = f.off;
    e["type"] = TYPENAMES[f.type];
    if (f.type == FT_U16 || f.type == FT_U32 || f.type == FT_I16 || f.type == FT_I32)
      e["be"] = f.be ? 1 : 0;
    if (f.type == FT_BIT) e["bit"] = f.bit;
    if (f.scale != 1.0f) e["scale"] = f.scale;
    if (f.unit.length()) e["unit"] = f.unit;
    if (f.enums.size())
    {
      JsonObject en = e["enum"].to<JsonObject>();
      for (auto &kv : f.enums) en[String(kv.first)] = kv.second;
    }
  }
  serializeJson(d, out);
}

// —— 解析 APP 下发的规则 JSON → gRule（不落盘）——
static bool applyRuleJson(const String &json, String &err)
{
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) { err = "bad json: " + String(e.c_str()); return false; }
  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) { err = "not an object"; return false; }

  Rule r;
  const char *nm = root["name"] | "";
  r.name = safeName(String(nm));
  if (!r.name.length()) { err = "missing name"; return false; }
  r.ver = (int)root["ver"] | 1;
  r.src = String((const char *)(root["src"] | ""));

  if (root["addr"].is<JsonObject>())
  {
    JsonObject a = root["addr"];
    const char *at = a["type"] | "u8";
    uint8_t tt = typeNameToEnum(at, true);
    if (tt == 0xFF) { err = "addr.type must be u8/u16"; return false; }
    r.multi = true;
    r.addrOff = (uint16_t)(a["off"] | 0);
    r.addrType = tt;
    r.addrBe = ((int)a["be"] | 0) != 0;
  }

  if (root["match"].is<JsonArray>())
  {
    JsonArray ma = root["match"];
    if (ma.size() > 4) { err = "match too many (max 4)"; return false; }
    for (JsonObject m : ma)
    {
      Rule::Match mm;
      mm.off = (uint16_t)(m["off"] | 0);
      JsonArray vals = m["val"];
      if (!vals || !vals.size()) { err = "match.val empty"; return false; }
      if (vals.size() > 8) { err = "match.val too many (max 8)"; return false; }
      for (int v : vals) mm.vals.push_back((uint8_t)v);
      r.match.push_back(mm);
    }
  }

  JsonArray fs = root["fields"];
  if (!fs || !fs.size()) { err = "missing fields"; return false; }
  if (fs.size() > 24) { err = "too many fields (max 24)"; return false; }
  for (JsonObject fo : fs)
  {
    Field f;
    f.name = String((const char *)(fo["name"] | ""));
    if (!f.name.length()) { err = "field missing name"; return false; }
    if (f.name.length() > 32) f.name = f.name.substring(0, 32);
    f.off = (uint16_t)(fo["off"] | 0);
    const char *ty = fo["type"] | "u16";
    uint8_t tt = typeNameToEnum(ty, false);
    if (tt == 0xFF) { err = "field type bad: " + String(ty); return false; }
    f.type = tt;
    f.be = ((int)fo["be"] | 0) != 0;
    f.bit = (uint8_t)(fo["bit"] | 0);
    if (f.type == FT_BIT && f.bit > 7) { err = "bit must be 0-7"; return false; }
    f.scale = (float)((double)(fo["scale"] | 1.0));
    if (f.scale == 0.0f) f.scale = 1.0f;
    f.unit = String((const char *)(fo["unit"] | ""));
    if (fo["enum"].is<JsonObject>())
    {
      JsonObject en = fo["enum"];
      if (en.size() > 8) { err = "enum too many (max 8)"; return false; }
      for (JsonPair kv : en)
      {
        int32_t k = (int32_t)atoi(kv.key().c_str());
        f.enums.push_back({k, String((const char *)(kv.value() | ""))});
      }
    }
    r.fields.push_back(f);
  }

  gRule = r;
  gHas = true;
  gDevs.clear();
  return true;
}

// —— TF 存取 ——
static String rulePath(const String &name) { return "/rules/" + name + ".json"; }

static bool readFile(const String &path, String &out)
{
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  out = f.readString();
  f.close();
  return out.length() > 0;
}

static bool saveActive()
{
  if (!lsEnsureMount()) return false;   // 无卡：只在 RAM 生效
  SD_MMC.mkdir("/rules");
  String out;
  serializeRule(gRule, out);
  File f = SD_MMC.open(rulePath(gRule.name), FILE_WRITE);
  if (!f) { RELOG("save fail %s\n", gRule.name.c_str()); return false; }
  f.print(out);
  f.close();
  File a = SD_MMC.open("/rules/_active.txt", FILE_WRITE);
  if (a) { a.print(gRule.name); a.close(); }
  RELOG("saved /rules/%s.json ver=%d fields=%d multi=%d\n",
        gRule.name.c_str(), gRule.ver, (int)gRule.fields.size(), (int)gRule.multi);
  return true;
}

void re::reBegin()
{
  if (!lsEnsureMount()) return;
  String an;
  if (!readFile("/rules/_active.txt", an)) return;
  an.trim();
  if (!an.length()) return;
  String json;
  if (!readFile(rulePath(an), json)) return;
  String err;
  if (applyRuleJson(json, err))
  {
    RELOG("restored rule %s (%d fields)\n", gRule.name.c_str(), (int)gRule.fields.size());
  }
  else
  {
    gHas = false;
    RELOG("restore fail %s: %s\n", an.c_str(), err.c_str());
  }
}

bool re::setRuleJson(const String &json, String &err)
{
  if (!applyRuleJson(json, err)) return false;
  saveActive();
  return true;
}

bool re::getRuleJson(const String &name, String &out)
{
  // 先读文件（可能是历史版本），读不到且是当前规则则序列化 RAM
  if (lsMounted() || lsEnsureMount())
  {
    if (readFile(rulePath(name), out)) return true;
  }
  if (gHas && name == gRule.name) { serializeRule(gRule, out); return true; }
  return false;
}

bool re::delRule(const String &name)
{
  if (!lsEnsureMount()) return false;
  String sn = safeName(name);
  if (!sn.length()) return false;
  return SD_MMC.remove(rulePath(sn));
}

void re::listNames(std::vector<String> &out)
{
  if (lsEnsureMount())
  {
    SD_MMC.mkdir("/rules");
    File dir = SD_MMC.open("/rules");
    if (dir)
    {
      for (;;)
      {
        File f = dir.openNextFile();
        if (!f) break;
        String n = String(f.name());
        f.close();
        if (!n.endsWith(".json") || n.startsWith("_")) continue;
        out.push_back(n.substring(0, n.length() - 5));
      }
      dir.close();
    }
  }
  // 无卡或文件不在时，当前激活规则也列进去
  if (gHas)
  {
    bool found = false;
    for (auto &n : out)
      if (n == gRule.name) { found = true; break; }
    if (!found) out.push_back(gRule.name);
  }
}

// —— 取值与格式化 ——
static bool readRaw(const uint8_t *d, int n, uint16_t off, uint8_t size, bool be, uint8_t type, uint8_t bit, int64_t &ival, float &fval, bool &isF)
{
  if (off + size > (uint16_t)n) return false;
  isF = (type == FT_F32);
  if (isF)
  {
    uint32_t u = 0;
    memcpy(&u, d + off, 4);
    memcpy(&fval, &u, 4);
    return true;
  }
  if (type == FT_BIT)
  {
    ival = (d[off] >> bit) & 1;
    return true;
  }
  uint64_t u = 0;
  if (size == 1) u = d[off];
  else if (size == 2) u = be ? ((uint16_t)d[off] << 8 | d[off + 1]) : ((uint16_t)d[off + 1] << 8 | d[off]);
  else if (size == 4)
    u = be ? ((uint32_t)d[off] << 24 | (uint32_t)d[off + 1] << 16 | (uint32_t)d[off + 2] << 8 | d[off + 3])
           : ((uint32_t)d[off + 3] << 24 | (uint32_t)d[off + 2] << 16 | (uint32_t)d[off + 1] << 8 | d[off]);
  switch (type)
  {
  case FT_I8:  ival = (int8_t)u; break;
  case FT_I16: ival = (int16_t)u; break;
  case FT_I32: ival = (int32_t)u; break;
  default:     ival = (int64_t)u; break;
  }
  return true;
}

// scale 决定小数位数：0.1 → 1 位，0.01 → 2 位
static int scaleDecimals(float scale)
{
  if (scale >= 1.0f || scale <= 0.0f) return 0;
  int dec = 0;
  float s = scale;
  while (s < 0.999f && dec < 4) { s *= 10.0f; dec++; }
  return dec;
}

static String fmtValue(const re::Field &f, int64_t ival, float fval, bool isF)
{
  // 枚举优先（仅整数型）
  if (!isF && f.enums.size())
  {
    for (auto &kv : f.enums)
      if (kv.first == (int32_t)ival) return kv.second;
  }
  if (isF)
  {
    float v = fval * f.scale;
    char b[24];
    snprintf(b, sizeof(b), "%.2f", (double)v);
    return String(b);
  }
  double v = (double)ival * (double)f.scale;
  int dec = scaleDecimals(f.scale);
  char b[32];
  snprintf(b, sizeof(b), "%.*f", dec, v);
  return String(b);
}

static int findDev(int addr)
{
  for (int i = 0; i < (int)gDevs.size(); i++)
    if (gDevs[i].addr == addr) return i;
  if ((int)gDevs.size() >= DEV_MAX) return -1;
  DevState ds;
  ds.addr = addr;
  ds.ms = 0;
  ds.vals.assign(gRule.fields.size(), String());
  gDevs.push_back(ds);
  return (int)gDevs.size() - 1;
}

void re::tapFrame(bool rx, uint32_t ms, const uint8_t *d, int n, DecResult &out)
{
  out.ok = false;
  if (!rx || !gHas || !d || n <= 0) return;

  for (auto &mm : gRule.match)
  {
    if (mm.off >= (uint16_t)n) return;
    bool hit = false;
    for (auto v : mm.vals)
      if (d[mm.off] == v) { hit = true; break; }
    if (!hit) return;
  }

  int addr = 0;
  if (gRule.multi)
  {
    int64_t iv;
    float fv;
    bool isF;
    uint8_t sz = TYPESIZE[gRule.addrType];
    if (!readRaw(d, n, gRule.addrOff, sz, gRule.addrBe, gRule.addrType, 0, iv, fv, isF)) return;
    addr = (int)iv;
  }
  int di = findDev(addr);
  if (di < 0) return;

  DevState &dv = gDevs[di];
  dv.ms = ms;
  String line, csv;
  for (int i = 0; i < (int)gRule.fields.size(); i++)
  {
    re::Field &f = gRule.fields[i];
    int64_t ival;
    float fval;
    bool isF;
    if (i) csv += ",";
    if (readRaw(d, n, f.off, TYPESIZE[f.type], f.be, f.type, f.bit, ival, fval, isF))
    {
      String s = fmtValue(f, ival, fval, isF);
      if (f.unit.length() && !f.enums.size()) s += f.unit;
      dv.vals[i] = s;
      if (i) line += " ";
      line += f.name + " " + s;
      csv += "\"" + s + "\"";
    }
    else
    {
      dv.vals[i] = "";
      csv += "\"\"";
    }
  }

  out.ok = true;
  out.dev = addr;
  out.line = line;
  out.csvVals = csv;

  if (gPush)
  {
    String ev = String("{\"evt\":\"dec\",\"t\":") + ms + ",\"rule\":\"" + gRule.name + "\"";
    if (gRule.multi) ev += ",\"dev\":" + String(addr);
    ev += ",\"vals\":[";
    for (int i = 0; i < (int)gRule.fields.size(); i++)
    {
      if (i) ev += ",";
      ev += dv.vals[i].length() ? ("\"" + dv.vals[i] + "\"") : "null";
    }
    ev += "]}\n";
    slSendRaw((const uint8_t *)ev.c_str(), ev.length());
  }
}

// —— 屏幕解码页 ——
int re::devCount() { return (int)gDevs.size(); }
int re::devAddrAt(int i) { return (i >= 0 && i < (int)gDevs.size()) ? gDevs[i].addr : 0; }
int re::fieldCount() { return gHas ? (int)gRule.fields.size() : 0; }
const re::Field &re::fieldAt(int i) { return gRule.fields[i]; }

String re::curVal(int dev, int fieldIdx)
{
  for (auto &dv : gDevs)
    if (dv.addr == dev)
      return (fieldIdx >= 0 && fieldIdx < (int)dv.vals.size()) ? dv.vals[fieldIdx] : String();
  return String();
}

uint32_t re::curMs(int dev)
{
  for (auto &dv : gDevs)
    if (dv.addr == dev) return dv.ms;
  return 0;
}
