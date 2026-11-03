#include "app_ctrl.h"
#include "serial_link.h"
#include "ble_relay.h"
#include "rule_engine.h"
#include "log_store.h"
#include "ota_update.h"
#include "cfg_table.h"
#include "sim_gen.h"
#include "log_tee.h"
#include <ArduinoJson.h>
#include <vector>

#define ACLOG(fmt, ...)                     \
  do                                        \
  {                                         \
    logTeePrintf("[ctrl] " fmt, ##__VA_ARGS__); \
    Serial0.printf("[ctrl] " fmt, ##__VA_ARGS__); \
  } while (0)

static std::vector<uint8_t> lineBuf;   // 控制行拼行缓冲（rule.set 可到 8KB）
static uint32_t lineStartMs = 0;       // 拼行开始时刻（超时自愈用）

static void sendLine(const String &s)
{
  String o = s + "\n";
  slSendRaw((const uint8_t *)o.c_str(), o.length());
}

static String esc(const String &s) { return cfgJsonEsc(s); }

static void rspOk(const String &extra = "")
{
  sendLine(String("{\"rsp\":\"ok\"") + (extra.length() ? "," + extra : "") + "}");
}

static void rspErr(const String &why) { sendLine("{\"rsp\":\"err\",\"why\":\"" + esc(why) + "\"}"); }

static void handleCmd(const char *cmd, JsonDocument &doc)
{
  if (!strcmp(cmd, "info"))
  {
    String s = String("{\"rsp\":\"info\",\"fw\":\"") + otaVersion() + "\"" +
               ",\"relay\":" + (relayConnected() ? "true" : "false") +
               ",\"conn\":" + (slActive() ? "true" : "false") +
               ",\"rule\":\"" + esc(re::ruleName()) + "\"" +
               ",\"log\":" + (lsActive() ? "true" : "false") + "}";
    sendLine(s);
    return;
  }
  if (!strcmp(cmd, "rule.set"))
  {
    String body;
    serializeJson(doc["rule"], body);
    String err;
    if (body.length() && body != "null" && re::setRuleJson(body, err)) rspOk();
    else rspErr(err.length() ? err : "missing rule");
    return;
  }
  if (!strcmp(cmd, "rule.list"))
  {
    std::vector<String> names;
    re::listNames(names);
    String s = "{\"rsp\":\"rule.list\",\"names\":[";
    for (size_t i = 0; i < names.size(); i++)
    {
      if (i) s += ",";
      s += "\"" + esc(names[i]) + "\"";
    }
    s += "]}";
    sendLine(s);
    return;
  }
  if (!strcmp(cmd, "rule.get"))
  {
    String name = String((const char *)(doc["name"] | ""));
    String out;
    if (name.length() && re::getRuleJson(name, out))
      sendLine("{\"rsp\":\"rule.get\",\"rule\":" + out + "}");
    else rspErr("rule not found");
    return;
  }
  if (!strcmp(cmd, "rule.del"))
  {
    String name = String((const char *)(doc["name"] | ""));
    if (name.length() && re::delRule(name)) rspOk();
    else rspErr("del fail");
    return;
  }
  if (!strcmp(cmd, "log.stop") || !strcmp(cmd, "log.start"))
  {
    if (cmd[4] == 's')   // log.stop
    {
      String sess = lsSession();
      lsStop();
      rspOk(sess.length() ? ("\"session\":\"" + esc(sess) + "\"") : "");
    }
    else
    {
      bool ok = lsStart("app");
      if (ok) rspOk("\"session\":\"" + esc(lsSession()) + "\"");
      else rspErr("no sd card");
    }
    return;
  }
  if (!strcmp(cmd, "dec.on") || !strcmp(cmd, "dec.off"))
  {
    bool on = (cmd[4] == 'o');   // dec.on
    re::setPush(on);
    int i = cfgIndexOf("dec.push");
    if (i >= 0)
    {
      String e;
      cfgSet(cfgAt(i), on ? "1" : "0", e);
    }
    rspOk();
    return;
  }
  if (!strcmp(cmd, "sim.on") || !strcmp(cmd, "sim.off"))
  {
    bool on = (cmd[4] == 'o');   // sim.on
    sim::set(on);
    int i = cfgIndexOf("sim.io");
    if (i >= 0)
    {
      String e;
      cfgSet(cfgAt(i), on ? "1" : "0", e);
    }
    rspOk();
    return;
  }
  rspErr("unknown cmd: " + String(cmd));
}

static void handleLine(const uint8_t *d, int n)
{
  String s;
  s.concat((const char *)d, n);
  s.trim();
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, s);
  if (e) { rspErr("bad json"); return; }
  const char *cmd = doc["cmd"] | "";
  if (!cmd[0]) { rspErr("missing cmd"); return; }
  ACLOG("cmd %s\n", cmd);
  handleCmd(cmd, doc);
}

// 控制行钩子（serial_link 在记帧/透传钩子之前调）：
// 返回 true = 本包已按控制行消费（不进报文记录、不走透传钩子）；
// 返回 false = 普通透传包，交回上层（flAddRx + 透传钩子）。
// 拼行规则：以 { 开头收行直到 \n（8KB 上限）；行间残留的非 { 字节直接透传。
static bool ctrlHook(const uint8_t *d, int n)
{
  if (n <= 0) return false;
  // 拼行超时自愈：半截行卡住 500ms 就了结（APP 忘发 \n / 写半截后不再来数据）。
  // 不了结的话 lineBuf 永远非空，之后所有手机数据都会被当成控制行续包吞掉。
  if (lineBuf.size() && millis() - lineStartMs > 500)
  {
    String s;
    s.concat((const char *)lineBuf.data(), lineBuf.size());
    s.trim();
    JsonDocument probe;
    bool isCmd = (deserializeJson(probe, s) == DeserializationError::Ok) && probe["cmd"].is<const char *>();
    if (isCmd)
      handleLine(lineBuf.data(), (int)lineBuf.size());   // 完整 JSON 命令但没带 \n：照样执行
    else
      for (auto b : lineBuf) relayPassThrough(&b, 1);    // 不是命令：整段透传放行
    lineBuf.clear();
    ACLOG("line timeout, cmd=%d len=%d\n", (int)isCmd, (int)s.length());
  }
  if (!lineBuf.size() && d[0] != '{') return false;
  for (int i = 0; i < n; i++)
  {
    uint8_t c = d[i];
    if (!lineBuf.size() && c != '{')
    {
      relayPassThrough(&c, 1);   // 行结束后残留的透传字节（罕见）
      continue;
    }
    if (!lineBuf.size()) lineStartMs = millis();
    lineBuf.push_back(c);
    if (c == '\n')
    {
      handleLine(lineBuf.data(), (int)lineBuf.size());
      lineBuf.clear();
    }
    else if (lineBuf.size() > 8200)
    {
      for (auto b : lineBuf) relayPassThrough(&b, 1);   // 超限不是合法控制行，整段放行
      lineBuf.clear();
    }
  }
  return true;
}

void acInit()
{
  slSetCtrlHook(ctrlHook);
  ACLOG("init ok\n");
}
