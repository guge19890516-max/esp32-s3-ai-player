#include "cfg_table.h"
#include "config_store.h"
#include "ota_update.h"
#include "serial_link.h"
#include "bms_client.h"
#include "rec_store.h"

#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>

// 预置 AI 厂商（OpenAI 兼容的接口基础地址）；"自定义" 走手动填 URL
struct VendorPreset { const char *name; const char *url; };
static const VendorPreset VENDORS[] = {
    {"DeepSeek", "https://api.deepseek.com"},
    {"Kimi", "https://api.moonshot.cn/v1"},
    {"智谱 GLM", "https://open.bigmodel.cn/api/paas/v4"},
    {"通义千问", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
    {"豆包", "https://ark.cn-beijing.volces.com/api/v3"},
    {"硅基流动", "https://api.siliconflow.cn/v1"},
    {"OpenAI", "https://api.openai.com/v1"},
};
#define VENDOR_N (sizeof(VENDORS) / sizeof(VENDORS[0]))

static const char *vendorUrl(const String &name)
{
  for (unsigned i = 0; i < VENDOR_N; i++)
    if (name == VENDORS[i].name) return VENDORS[i].url;
  return nullptr;
}

// 唯一设置表：加一项设置 = 加一行。
// 同一 group 的行必须连续排列（设置 JSON 按此分组）。
// 旧字段（net.* / ai.* / ota.*）映射到 Config cfg，仍走 cfgSaveWiFi/cfgSaveApi/cfgSaveOtaUrl；
// 新字段统一存 NVS 命名空间 "cfg"，键即 CfgItem.key（≤15 字符）。
static const CfgItem TABLE[] = {
    {"net.ssid", "网络", "WiFi 名称", CT_STR, 0, 0, nullptr, 0, ""},
    {"net.pass", "网络", "WiFi 密码", CT_STR, 0, 0, nullptr, CFG_SECRET, ""},
    {"ai.vendor", "AI 接口", "AI 厂商", CT_ENUM, 0, 0, "DeepSeek|Kimi|智谱 GLM|通义千问|豆包|硅基流动|OpenAI|自定义", 0, "自定义"},
    {"ai.url", "AI 接口", "接口地址", CT_STR, 0, 0, nullptr, 0, ""},
    {"ai.key", "AI 接口", "API Key", CT_STR, 0, 0, nullptr, CFG_SECRET, ""},
    {"ai.model", "AI 接口", "模型名称", CT_STR, 0, 0, nullptr, 0, ""},
    {"ota.url", "固件升级", "URL 升级地址", CT_STR, 0, 0, nullptr, 0, ""},
    {"web.pin", "安全", "网页需要 PIN", CT_BOOL, 0, 0, nullptr, 0, "0"},
    {"log.http", "调试", "网页日志", CT_BOOL, 0, 0, nullptr, 0, "1"},
    // BT04E 蓝牙模块的 AT 口：按需开关 UART2，改完即存即生效（无需重启）
    {"btmod.tx", "蓝牙模块", "AT 口 TX 引脚", CT_INT, 1, 48, nullptr, 0, "41"},
    {"btmod.rx", "蓝牙模块", "AT 口 RX 引脚", CT_INT, 1, 48, nullptr, 0, "42"},
    {"btmod.baud", "蓝牙模块", "AT 口波特率", CT_ENUM, 0, 0, "2400|4800|9600|19200|38400|57600|115200", 0, "9600"},
    // 串口收发编码：发送按 txenc 编码上链路，接收显示按 rxenc 解码（HEX 模式不受影响）
    {"serial.txenc", "编码", "发送编码", CT_ENUM, 0, 0, "UTF-8|ASCII|GBK|GB2312|UNICODE", 0, "UTF-8"},
    {"serial.rxenc", "编码", "接收编码", CT_ENUM, 0, 0, "UTF-8|ASCII|GBK|GB2312|UNICODE", 0, "UTF-8"},
    {"serial.bom", "编码", "UNICODE 带 BOM", CT_BOOL, 0, 0, nullptr, 0, "0"},
    // 蓝牙中继：ESP32 作主机连蓝牙串口模块收报文，转发屏幕 + 手机
    {"relay.on", "蓝牙中继", "中继开关", CT_BOOL, 0, 0, nullptr, 0, "0"},
    {"relay.name", "蓝牙中继", "目标名称前缀", CT_STR, 0, 0, nullptr, 0, "BT04E"},
    {"relay.pass", "蓝牙中继", "串口透传(手机→模块)", CT_BOOL, 0, 0, nullptr, 0, "0"},
    {"relay.tx", "蓝牙中继", "屏幕发送目标", CT_ENUM, 0, 0, "手机+模块|仅手机|仅模块", 0, "手机+模块"},
    // 协议解码：APP 下发规则（docs/app-protocol.md），事件推送到手机、解码结果落盘
    {"dec.push", "协议解码", "解码事件推送", CT_BOOL, 0, 0, nullptr, 0, "0"},
    {"dec.csv", "协议解码", "decoded.csv 落盘", CT_BOOL, 0, 0, nullptr, 0, "0"},
    {"sim.io", "协议解码", "模拟开关量帧", CT_BOOL, 0, 0, nullptr, 0, "0"},
};
#define TABLE_N (sizeof(TABLE) / sizeof(TABLE[0]))

static bool gPinReq = false;   // web.pin 当前值（cfgInit 时从 NVS 载入）
static uint16_t gPin = 0;      // 本次开机随机 PIN
static String gAiVendor = "自定义";  // 当前厂商
static String gModels;               // 当前厂商的模型列表缓存（'\n' 分隔）
// 中继开关走内存镜像：relayPoll 每圈都要查，直读 NVS 太贵
static bool gRelayOn = false;
static bool gRelayPass = false;
// 编码设置内存镜像：收发路径每帧都要查
static String gTxEnc = "UTF-8";
static String gRxEnc = "UTF-8";
static bool gUniBom = false;

void cfgInit()
{
  Preferences p;
  p.begin("cfg", true);
  gPinReq = p.getBool("web.pin", false);
  gAiVendor = p.getString("ai.vendor", "自定义");
  gRelayOn = p.getBool("relay.on", false);
  gRelayPass = p.getBool("relay.pass", false);
  gTxEnc = p.getString("serial.txenc", "UTF-8");
  gRxEnc = p.getString("serial.rxenc", "UTF-8");
  gUniBom = p.getBool("serial.bom", false);
  p.end();
  // 记住过的厂商恢复其 url/key/model/模型列表
  if (gAiVendor != "自定义")
  {
    AiRec r;
    if (aiRecGet(gAiVendor, r))
    {
      if (r.url.length()) cfg.url = apiNormalizeBase(r.url);   // 历史坏记录（完整端点）也兜底
      if (r.key.length()) cfg.key = r.key;
      if (r.model.length()) cfg.model = r.model;
      gModels = r.models;
    }
    else
    {
      const char *u = vendorUrl(gAiVendor);
      if (u) cfg.url = u;
    }
  }
  gPin = (uint16_t)(esp_random() % 10000);
}

int cfgCount() { return (int)TABLE_N; }
const CfgItem &cfgAt(int i) { return TABLE[i]; }

int cfgIndexOf(const char *key)
{
  for (int i = 0; i < cfgCount(); i++)
    if (!strcmp(TABLE[i].key, key)) return i;
  return -1;
}

static String maskSecret(const String &v)
{
  if (v.length() <= 4) return "****";
  return v.substring(0, 3) + "****" + v.substring(v.length() - 2);
}

String cfgGetRaw(const CfgItem &it)
{
  if (!strcmp(it.key, "ai.vendor")) return gAiVendor;
  if (!strcmp(it.key, "net.ssid")) return cfg.ssid;
  if (!strcmp(it.key, "net.pass")) return cfg.wpass;
  if (!strcmp(it.key, "ai.url")) return cfg.url;
  if (!strcmp(it.key, "ai.key")) return cfg.key;
  if (!strcmp(it.key, "ai.model")) return cfg.model;
  if (!strcmp(it.key, "ota.url")) return cfg.otaUrl;
  if (!strcmp(it.key, "web.pin")) return gPinReq ? "1" : "0";
  if (!strcmp(it.key, "log.http"))
  {
    Preferences p;
    p.begin("cfg", true);
    bool on = p.getBool("log.http", true);
    p.end();
    return on ? "1" : "0";
  }
  if (!strcmp(it.key, "btmod.tx") || !strcmp(it.key, "btmod.rx") || !strcmp(it.key, "btmod.baud"))
  {
    Preferences p;
    p.begin("cfg", true);
    String def = !strcmp(it.key, "btmod.tx") ? "41" : !strcmp(it.key, "btmod.rx") ? "42" : "9600";
    String v = p.getString(it.key, def);
    p.end();
    return v;
  }
  if (!strcmp(it.key, "relay.on")) return gRelayOn ? "1" : "0";
  if (!strcmp(it.key, "relay.pass")) return gRelayPass ? "1" : "0";
  if (!strcmp(it.key, "serial.txenc")) return gTxEnc;
  if (!strcmp(it.key, "serial.rxenc")) return gRxEnc;
  if (!strcmp(it.key, "serial.bom")) return gUniBom ? "1" : "0";
  if (!strcmp(it.key, "relay.name") || !strcmp(it.key, "relay.tx"))
  {
    Preferences p;
    p.begin("cfg", true);
    String def = !strcmp(it.key, "relay.name") ? "BT04E" : "手机+模块";
    String v = p.getString(it.key, def);
    p.end();
    return v;
  }
  return String();
}

String cfgGetShow(const CfgItem &it)
{
  String v = cfgGetRaw(it);
  if (it.flags & CFG_SECRET) return maskSecret(v);
  return v;
}

bool cfgSet(const CfgItem &it, const String &val, String &err)
{
  String v = val;
  v.trim();
  switch (it.type)
  {
  case CT_STR:
    if (v.length() > 63) { err = "内容太长（最多 63 字）"; return false; }
    break;
  case CT_INT:
  {
    long n = v.toInt();
    if (!v.length() || String(n) != v) { err = "需要整数"; return false; }
    if (n < it.minv || n > it.maxv) { err = "超出范围 " + String(it.minv) + "~" + String(it.maxv); return false; }
    v = String(n);
    break;
  }
  case CT_BOOL:
    if (v == "1" || v == "true" || v == "on" || v == "开" || v == "开启") v = "1";
    else if (v == "0" || v == "false" || v == "off" || v == "关" || v == "关闭") v = "0";
    else { err = "取值应为 开/关"; return false; }
    break;
  case CT_ENUM:
  {
    // options 形如 "a|b|c"，取值必须命中其中一项
    String opts = it.options;
    bool hit = false;
    int i = 0;
    while (i <= opts.length())
    {
      int p = opts.indexOf('|', i);
      if (p < 0) p = opts.length();
      if (opts.substring(i, p) == v) { hit = true; break; }
      i = p + 1;
    }
    if (!hit) { err = "取值应为: " + opts; return false; }
    break;
  }
  }

  // 厂商切换：CT_ENUM 校验已通过，这里按预置套 URL，并从记录恢复 key/model/模型列表
  if (!strcmp(it.key, "ai.vendor"))
  {
    gAiVendor = v;
    Preferences p;
    p.begin("cfg", false);
    p.putString("ai.vendor", v);
    p.end();
    if (v != "自定义")
    {
      const char *u = vendorUrl(v);
      if (u) cfg.url = u;
      AiRec r;
      if (aiRecGet(v, r))
      {
        if (r.key.length()) cfg.key = r.key;
        if (r.model.length()) cfg.model = r.model;
        gModels = r.models;
      }
      cfgSaveApi();
    }
    else gModels = "";
    return true;
  }

  if (!strcmp(it.key, "net.ssid")) { cfg.ssid = v; cfgSaveWiFi(); }
  else if (!strcmp(it.key, "net.pass")) { cfg.wpass = v; cfgSaveWiFi(); }
  else if (!strcmp(it.key, "ai.url")) { cfg.url = apiNormalizeBase(v); cfgSaveApi(); }
  else if (!strcmp(it.key, "ai.key")) { cfg.key = v; cfgSaveApi(); }
  else if (!strcmp(it.key, "ai.model")) { cfg.model = v; cfgSaveApi(); }
  else if (!strcmp(it.key, "ota.url")) { cfg.otaUrl = v; cfgSaveOtaUrl(); }
  else if (!strcmp(it.key, "web.pin"))
  {
    gPinReq = (v == "1");
    Preferences p;
    p.begin("cfg", false);
    p.putBool("web.pin", gPinReq);
    p.end();
  }
  else if (!strcmp(it.key, "log.http"))
  {
    Preferences p;
    p.begin("cfg", false);
    p.putBool("log.http", v == "1");
    p.end();
  }
  else if (!strcmp(it.key, "btmod.tx") || !strcmp(it.key, "btmod.rx") || !strcmp(it.key, "btmod.baud"))
  {
    Preferences p;
    p.begin("cfg", false);
    p.putString(it.key, v);
    p.end();
  }
  else if (!strcmp(it.key, "relay.on"))
  {
    gRelayOn = (v == "1");
    Preferences p;
    p.begin("cfg", false);
    p.putBool(it.key, gRelayOn);
    p.end();
  }
  else if (!strcmp(it.key, "relay.pass"))
  {
    gRelayPass = (v == "1");
    Preferences p;
    p.begin("cfg", false);
    p.putBool(it.key, gRelayPass);
    p.end();
  }
  else if (!strcmp(it.key, "relay.name") || !strcmp(it.key, "relay.tx"))
  {
    Preferences p;
    p.begin("cfg", false);
    p.putString(it.key, v);
    p.end();
  }
  else if (!strcmp(it.key, "serial.txenc"))
  {
    gTxEnc = v;
    Preferences p;
    p.begin("cfg", false);
    p.putString(it.key, v);
    p.end();
  }
  else if (!strcmp(it.key, "serial.rxenc"))
  {
    gRxEnc = v;
    Preferences p;
    p.begin("cfg", false);
    p.putString(it.key, v);
    p.end();
  }
  else if (!strcmp(it.key, "serial.bom"))
  {
    gUniBom = (v == "1");
    Preferences p;
    p.begin("cfg", false);
    p.putBool(it.key, gUniBom);
    p.end();
  }
  else { err = "未映射的配置项"; return false; }
  return true;
}

String cfgTxEnc() { return gTxEnc; }
String cfgRxEnc() { return gRxEnc; }
bool cfgUniBom() { return gUniBom; }

String cfgJsonEsc(const String &s)
{
  String o;
  o.reserve(s.length() + 8);
  for (int i = 0; i < s.length(); i++)
  {
    char c = s[i];
    switch (c)
    {
    case '"': o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if ((uint8_t)c < 0x20)
      {
        char b[8];
        snprintf(b, sizeof(b), "\\u%04x", c);
        o += b;
      }
      else o += c;
    }
  }
  return o;
}

void cfgBuildSettingsJson(String &out)
{
  out.reserve(1024);
  out = "{\"api\":1,\"groups\":[";
  const char *curG = nullptr;
  bool firstGroup = true;
  for (int i = 0; i < cfgCount(); i++)
  {
    const CfgItem &it = TABLE[i];
    if (!curG || strcmp(it.group, curG) != 0)
    {
      if (curG) out += "]}";
      if (!firstGroup) out += ",";
      out += "{\"g\":\"" + cfgJsonEsc(it.group) + "\",\"items\":[";
      curG = it.group;
      firstGroup = false;
    }
    else out += ",";

    const char *t = (it.type == CT_STR) ? "str" : (it.type == CT_INT) ? "int"
                                   : (it.type == CT_BOOL)              ? "bool"
                                                                       : "enum";
    out += "{\"k\":\"" + cfgJsonEsc(it.key) + "\",\"l\":\"" + cfgJsonEsc(it.label) +
           "\",\"t\":\"" + t + "\",\"v\":\"" + cfgJsonEsc(cfgGetShow(it)) + "\"";
    if (it.flags & CFG_SECRET) out += ",\"secret\":true";
    if (it.type == CT_INT) out += ",\"min\":" + String(it.minv) + ",\"max\":" + String(it.maxv);
    if (it.type == CT_ENUM) out += ",\"o\":\"" + cfgJsonEsc(it.options) + "\"";
    out += "}";
  }
  if (curG) out += "]}";
  out += "]}";
}

void cfgBuildStatusJson(String &out)
{
  bool up = (WiFi.status() == WL_CONNECTED);
  out.reserve(512);
  out = "{\"fw\":\"" FW_VERSION "\"";
  out += ",\"slot\":\"" + cfgJsonEsc(otaSlot()) + "\"";
  out += ",\"ip\":\"" + (up ? WiFi.localIP().toString() : String()) + "\"";
  out += ",\"ssid\":\"" + cfgJsonEsc(up ? WiFi.SSID() : String()) + "\"";
  out += ",\"rssi\":" + String(up ? WiFi.RSSI() : 0);
  out += ",\"heap\":" + String((uint32_t)ESP.getFreeHeap());
  out += ",\"maxblk\":" + String((uint32_t)ESP.getMaxAllocHeap());
  out += ",\"ble\":{\"conn\":";
  out += slBleConnected() ? "true" : "false";
  out += ",\"adv\":";
  out += slBleAdvActive() ? "true" : "false";
  out += ",\"name\":\"" + cfgJsonEsc(String(slDeviceName())) + "\"}";
  out += ",\"bms\":{\"ok\":";
  if (bms::hasData())
  {
    bms::BaseInfo I = bms::info();
    out += "true,\"soc\":" + String(I.soc);
    out += ",\"v\":" + String(I.totalVoltage, 2);
    out += ",\"a\":" + String(I.current, 2);
  }
  else out += "false";
  out += "}";
  out += ",\"pinreq\":";
  out += gPinReq ? "true" : "false";
  if (gPinReq)
  {
    char b[8];
    snprintf(b, sizeof(b), "%04u", (unsigned)gPin);
    out += ",\"pin\":\"";
    out += b;
    out += "\"";
  }
  out += "}";
}

bool cfgWebPinRequired() { return gPinReq; }
uint16_t cfgPin() { return gPin; }
String cfgVendorName() { return gAiVendor; }
String cfgModels() { return gModels; }
void cfgSetModels(const String &models) { gModels = models; }
bool cfgFlag(const char *key)
{
  int i = cfgIndexOf(key);
  return i >= 0 && cfgGetRaw(cfgAt(i)) == "1";
}
