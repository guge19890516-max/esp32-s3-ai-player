#include "ota_http.h"
#include "ota_update.h"
#include "cfg_table.h"
#include "bt_module.h"
#include "log_tee.h"

#include <WiFi.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define HDR_MAX 2048
#define BUF_MAX 4096

static WiFiServer *srv = nullptr;
static volatile bool writing = false;
static bool bound = false;
static String lastInfo;

static const char *reason(int code)
{
  switch (code)
  {
  case 200: return "OK";
  case 204: return "No Content";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 404: return "Not Found";
  case 409: return "Conflict";
  default: return "Error";
  }
}

static void reply(WiFiClient &c, int code, const char *body)
{
  c.printf("HTTP/1.1 %d %s\r\n", code, reason(code));
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
  c.print("Access-Control-Allow-Headers: Content-Type, X-Pin\r\n");
  // Chrome 的私有网络访问（localhost 页面访问内网 IP）需要这个响应头
  c.print("Access-Control-Allow-Private-Network: true\r\n");
  c.print("Access-Control-Max-Age: 86400\r\n");
  c.print("Content-Type: application/json; charset=utf-8\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %u\r\n\r\n", (unsigned)(body ? strlen(body) : 0));
  if (body && code != 204) c.print(body);
  c.flush();
}

static void replyText(WiFiClient &c, const char *body)
{
  c.printf("HTTP/1.1 200 OK\r\n");
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Access-Control-Allow-Private-Network: true\r\n");
  c.print("Content-Type: text/plain; charset=utf-8\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %u\r\n\r\n", (unsigned)(body ? strlen(body) : 0));
  if (body) c.print(body);
  c.flush();
}

static bool readHeaders(WiFiClient &c, String &head)
{
  head = "";
  head.reserve(HDR_MAX);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000)
  {
    while (c.available() > 0)
    {
      int v = c.read();
      if (v < 0) break;
      head += (char)v;
      if (head.endsWith("\r\n\r\n")) return true;
      if ((int)head.length() >= HDR_MAX) return false;
    }
    if (!c.connected() && c.available() <= 0) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

static int headerInt(const String &head, const char *name)
{
  String low = head;
  low.toLowerCase();
  String key = String(name);
  key.toLowerCase();
  int p = low.indexOf(key + ":");
  if (p < 0) return -1;
  int e = head.indexOf("\r\n", p);
  String v = head.substring(p + key.length() + 1, e < 0 ? head.length() : e);
  v.trim();
  return v.toInt();
}

static String headerStr(const String &head, const char *name)
{
  String low = head;
  low.toLowerCase();
  String key = String(name);
  key.toLowerCase();
  int p = low.indexOf(key + ":");
  if (p < 0) return String();
  int e = head.indexOf("\r\n", p);
  String v = head.substring(p + key.length() + 1, e < 0 ? head.length() : e);
  v.trim();
  return v;
}

// ---- 配置接口（/api/*）辅助 ----

static bool pinOkVal(const String &p)
{
  if (!cfgWebPinRequired()) return true;
  char want[8];
  snprintf(want, sizeof(want), "%04u", (unsigned)cfgPin());
  return p == want;
}

static int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String urlDecode(const String &s)
{
  String o;
  o.reserve(s.length());
  for (int i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length())
    {
      int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        o += (char)((hi << 4) | lo);
        i += 2;
      }
      else o += c;
    }
    else o += c;
  }
  return o;
}

static String formParam(const String &body, const char *name)
{
  int i = 0;
  int n = body.length();
  while (i < n)
  {
    int amp = body.indexOf('&', i);
    if (amp < 0) amp = n;
    int eq = body.indexOf('=', i);
    if (eq > i && eq < amp && body.substring(i, eq) == name)
      return urlDecode(body.substring(eq + 1, amp));
    i = amp + 1;
  }
  return String();
}

static bool readBody(WiFiClient &c, int len, String &body)
{
  body = "";
  if (len > 2048) return false;
  body.reserve(len);
  uint32_t t0 = millis();
  while ((int)body.length() < len && millis() - t0 < 5000)
  {
    while (c.available() > 0 && (int)body.length() < len)
    {
      int v = c.read();
      if (v < 0) break;
      body += (char)v;
    }
    if (!c.connected() && c.available() <= 0) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return (int)body.length() >= len;
}

static void apiSettingsPost(const String &body, int &code, String &out)
{
  String k = formParam(body, "k");
  String v = formParam(body, "v");
  if (!k.length())
  {
    code = 400;
    out = "{\"err\":\"缺少 k\"}";
    return;
  }
  for (int i = 0; i < cfgCount(); i++)
  {
    const CfgItem &it = cfgAt(i);
    if (k == it.key)
    {
      String err;
      if (cfgSet(it, v, err))
      {
        code = 200;
        out = "{\"ok\":true,\"v\":\"" + cfgJsonEsc(cfgGetShow(it)) + "\"}";
      }
      else
      {
        code = 400;
        out = "{\"err\":\"" + cfgJsonEsc(err) + "\"}";
      }
      return;
    }
  }
  code = 404;
  out = "{\"err\":\"未知的配置项\"}";
}

// 从 JSON body 里抠字符串字段（处理常见转义），抠不到返回空
static String jsonStrVal(const String &body, const char *key)
{
  String pat = String("\"") + key + "\"";
  int p = body.indexOf(pat);
  if (p < 0) return String();
  int colon = body.indexOf(':', p + pat.length());
  if (colon < 0) return String();
  int i = colon + 1;
  while (i < (int)body.length() && body[i] == ' ') i++;
  if (i >= (int)body.length() || body[i] != '"') return String();
  i++;
  String o;
  while (i < (int)body.length())
  {
    char c = body[i];
    if (c == '\\' && i + 1 < (int)body.length())
    {
      char n = body[i + 1];
      if (n == '"') o += '"';
      else if (n == '\\') o += '\\';
      else if (n == 'n') o += '\n';
      else if (n == 'r') o += '\r';
      else if (n == 't') o += '\t';
      else o += n;
      i += 2;
      continue;
    }
    if (c == '"') break;
    o += c;
    i++;
  }
  return o;
}

// POST /api/btmod：body {"cmd":"AT+BAUD7"}（也兼容表单 cmd=...）。
// 白名单：AT 开头、无控制字符、无引号反斜杠、≤64 字节；设置类命令成功后自动补发 AT+RESET
static void apiBtmodPost(const String &body, int &code, String &out)
{
  String cmd = formParam(body, "cmd");
  if (!cmd.length()) cmd = jsonStrVal(body, "cmd");
  cmd.trim();
  bool ok = cmd.length() >= 2 && cmd.length() <= 64 && cmd.startsWith("AT");
  for (unsigned i = 0; ok && i < cmd.length(); i++)
  {
    char c = cmd[i];
    if (c < 0x20 || c == '"' || c == '\\') ok = false;   // 防注入：命令里不允许控制字符/引号/反斜杠
  }
  if (!ok)
  {
    code = 400;
    out = "{\"err\":\"命令不合法：需 AT 开头、无控制字符、≤64 字节\"}";
    return;
  }
  logTeePrintf("[btmod] api POST cmd=%s（AT 仅在模块无蓝牙连接时有效）\n", cmd.c_str());
  String resp = btmodCmd(cmd, 900);
  bool done = resp.indexOf("OK") >= 0;
  if (done && btmodNeedsReset(cmd))
  {
    String r2 = btmodCmd("AT+RESET", 900);
    r2.trim();
    resp += "\n[已补发 AT+RESET] " + (r2.length() ? r2 : String("(无响应)"));
  }
  resp.trim();
  code = 200;
  out = "{\"ok\":" + String(done ? "true" : "false") + ",\"resp\":\"" + cfgJsonEsc(resp) + "\"}";
}

// 统一 API 分发：HTTP（handleClient）与蓝牙控制台（ota_update.cpp 的 conTask）共用。
bool otaApiDispatch(const String &method, const String &path, const String &pin,
                    const String &body, bool viaBle, int &code, String &out, bool &isText)
{
  isText = false;
  // caps 不做 PIN 校验：网页要先从它拿到 pinreq 才知道要不要输 PIN，
  // 且它只暴露固件版本与能力列表，不泄露配置。
  if (method == "GET" && path.startsWith("/api/caps"))
  {
    code = 200;
    out = "{\"api\":1,\"fw\":\"" FW_VERSION "\",\"caps\":[\"settings\",\"status\"],\"pinreq\":";
    out += cfgWebPinRequired() ? "true" : "false";
    out += "}";
    return true;
  }
  if (method == "GET" && path.startsWith("/api/settings"))
  {
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    cfgBuildSettingsJson(out);
    code = 200;
    return true;
  }
  if (method == "POST" && path.startsWith("/api/settings"))
  {
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    apiSettingsPost(body, code, out);
    return true;
  }
  if (method == "GET" && path.startsWith("/api/status"))
  {
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    cfgBuildStatusJson(out);
    code = 200;
    return true;
  }
  if (method == "GET" && path.startsWith("/api/wifi"))
  {
    // 附近的 WiFi 列表（网页配网点选用）：首次调用触发异步扫描并回 scanning=true，
    // 之后轮询直到出结果；scan=1 可在结果出来后再要一次新的
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    static uint32_t apScanT0 = 0;
    int sc = WiFi.scanComplete();
    bool running = (sc == WIFI_SCAN_RUNNING);
    if (!running && sc < 0)   // 从没扫过或上次失败 → 启动异步扫描
    {
      if (WiFi.status() != WL_CONNECTED)
      {
        // STA 在自动重连/连接中时扫描会返回 ESP_ERR_WIFI_STATE，先释放
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
      }
      sc = WiFi.scanNetworks(true, false, false, 120u);
      apScanT0 = millis();
      running = (sc == WIFI_SCAN_RUNNING);
      logTeePrintf("[http] wifi scan start -> %d\n", sc);
    }
    if (running && millis() - apScanT0 > 12000)
    {
      WiFi.scanDelete();
      out = "{\"scanning\":false,\"err\":\"扫描超时，请重试\"}";
    }
    else if (running)
      out = "{\"scanning\":true,\"aps\":[]}";
    else if (sc < 0)
      out = "{\"scanning\":false,\"err\":\"扫描失败，请重试\"}";
    else
    {
      // 去重（同名取最强）、按信号降序、最多 20 条，隐藏网络（空名）跳过
      if (sc > 40) sc = 40;
      String ss[40];
      int8_t rr[40];
      bool lk[40];
      int m = 0;
      for (int i = 0; i < sc; i++)
      {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;
        int8_t r = WiFi.RSSI(i);
        bool l = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        int j;
        for (j = 0; j < m; j++)
          if (ss[j] == s) break;
        if (j < m) { if (r > rr[j]) { rr[j] = r; lk[j] = l; } continue; }
        if (m >= 40) continue;
        ss[m] = s; rr[m] = r; lk[m] = l;
        m++;
      }
      for (int i = 1; i < m; i++)   // 插入排序按 RSSI 降序
      {
        String s = ss[i]; int8_t r = rr[i]; bool l = lk[i];
        int j = i - 1;
        while (j >= 0 && rr[j] < r)
        {
          ss[j + 1] = ss[j]; rr[j + 1] = rr[j]; lk[j + 1] = lk[j];
          j--;
        }
        ss[j + 1] = s; rr[j + 1] = r; lk[j + 1] = l;
      }
      if (m > 20) m = 20;
      out = "{\"scanning\":false,\"aps\":[";
      for (int i = 0; i < m; i++)
      {
        if (i) out += ",";
        out += "{\"s\":\"" + cfgJsonEsc(ss[i]) + "\",\"r\":" + String((int)rr[i]) +
               ",\"l\":" + (lk[i] ? "true" : "false") + "}";
      }
      out += "]}";
    }
    code = 200;
    return true;
  }
  if (method == "GET" && path.startsWith("/api/logs"))
  {
    // 「网页日志」开关管的是 HTTP 暴露面；蓝牙控制台不受它限制——
    // 设备没连 WiFi 时用户多半正需要日志排障。PIN 校验照常。
    if (!viaBle && !cfgFlag("log.http")) { code = 404; out = "{\"err\":\"网页日志已关闭\"}"; return true; }
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    if (path.indexOf("clear=1") >= 0)
    {
      logTeeClear();
      code = 200;
      out = "(已清空)\n";
      isText = true;
      return true;
    }
    out = logTeeDump(24000);
    if (!out.length()) out = "(暂无日志)\n";
    code = 200;
    isText = true;
    return true;
  }
  if (method == "GET" && path.startsWith("/api/btmod"))
  {
    // 查询 BT04E 模块信息（版本/MAC/名称/波特率/校验/停止位）
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    out = btmodInfo();
    code = 200;
    return true;
  }
  if (method == "POST" && path.startsWith("/api/btmod"))
  {
    // 给 BT04E 发 AT 命令（白名单 + PIN 校验，与 /api/settings 一致）
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    apiBtmodPost(body, code, out);
    return true;
  }
  if (method == "POST" && path.startsWith("/api/dbg"))
  {
    // AT 口原始收发调试：?on=1 打开 / ?off=1 关闭；否则 body {"data":"..","hex":0|1} 发原始字节
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    String q = path;
    int qi = q.indexOf('?');
    q = (qi >= 0) ? q.substring(qi + 1) : String();
    if (q.indexOf("on=1") >= 0)
    {
      bool ok = btmodDebug(true);
      code = ok ? 200 : 500;
      out = String("{\"ok\":") + (ok ? "true" : "false") + "}";
      return true;
    }
    if (q.indexOf("off=1") >= 0)
    {
      btmodDebug(false);
      code = 200;
      out = "{\"ok\":true}";
      return true;
    }
    String data = jsonStrVal(body, "data");
    bool hexMode = body.indexOf("\"hex\":1") >= 0 || body.indexOf("\"hex\":true") >= 0;
    String err;
    size_t n = btmodDebugSend(data, hexMode, err);
    if (err.length()) { code = 400; out = "{\"err\":\"" + cfgJsonEsc(err) + "\"}"; return true; }
    code = 200;
    out = String("{\"ok\":true,\"n\":") + String((unsigned)n) + "}";
    return true;
  }
  if (method == "GET" && path.startsWith("/api/dbg"))
  {
    // 取走调试口收到的字节（HEX），并报告调试口状态
    if (!pinOkVal(pin)) { code = 401; out = "{\"err\":\"需要 PIN\"}"; return true; }
    String rx = btmodDebugRecvHex();
    code = 200;
    out = String("{\"on\":") + (btmodDebugOn() ? "true" : "false");
    out += ",\"total\":" + String((unsigned long)btmodDebugRxCount());
    out += ",\"rx\":\"" + rx + "\"}";
    return true;
  }
  return false;
}

static void handlePost(WiFiClient &c, const String &head)
{
  int cl = headerInt(head, "Content-Length");
  if (cl <= 0)
  {
    reply(c, 400, "{\"err\":\"no content-length\"}");
    return;
  }
  if (otaBusy() || otaBleActive() || writing)
  {
    reply(c, 409, "{\"err\":\"busy\"}");
    return;
  }
  if ((size_t)cl < 65536 || (size_t)cl > otaTargetSize())
  {
    reply(c, 400, "{\"err\":\"bad size\"}");
    return;
  }
  if (!Update.begin((size_t)cl, U_FLASH))
  {
    reply(c, 400, "{\"err\":\"begin failed\"}");
    return;
  }

  uint8_t *buf = (uint8_t *)malloc(BUF_MAX);
  if (!buf)
  {
    Update.abort();
    reply(c, 400, "{\"err\":\"no mem\"}");
    return;
  }

  writing = true;
  otaReport(OST_RUNNING, "WiFi 推送升级", 0);
  logTeePrintf("[http] ota start %d bytes\n", cl);

  size_t got = 0;
  int lastPct = -1;
  uint32_t lastData = millis();
  while (got < (size_t)cl)
  {
    int avail = c.available();
    if (avail <= 0)
    {
      if (!c.connected()) break;
      if (millis() - lastData > 15000) break;
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }
    size_t want = (size_t)avail;
    if (want > BUF_MAX) want = BUF_MAX;
    int n = c.read(buf, want);
    if (n <= 0)
    {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    lastData = millis();
    if (Update.write(buf, (size_t)n) != (size_t)n) break;
    got += (size_t)n;
    int p = (int)((uint64_t)got * 100 / (uint64_t)cl);
    if (p != lastPct)
    {
      lastPct = p;
      otaReport(OST_RUNNING, "WiFi 写入 " + String((unsigned)(got / 1024)) + "/" +
                                 String((unsigned)((size_t)cl / 1024)) + " KB", p);
    }
  }
  free(buf);

  if (got != (size_t)cl || !Update.end(true) || !Update.isFinished())
  {
    Update.abort();
    writing = false;
    otaReport(OST_FAIL, "WiFi 推送失败", 0);
    logTeePrintf("[http] ota fail got=%u/%d\n", (unsigned)got, cl);
    reply(c, 400, "{\"err\":\"write failed\"}");
    return;
  }

  writing = false;
  otaReport(OST_DONE, "WiFi 推送升级完成，即将重启", 100);
  logTeePrintf("[http] ota done %u bytes\n", (unsigned)got);
  reply(c, 200, "{\"ok\":true}");
  otaRequestRestart(1500);
}

static void handleClient(WiFiClient &c)
{
  String head;
  if (!readHeaders(c, head))
  {
    reply(c, 400, "{\"err\":\"bad request\"}");
    return;
  }
  if (head.startsWith("OPTIONS"))
  {
    reply(c, 204, nullptr);
  }
  else if (head.startsWith("GET /info"))
  {
    reply(c, 200, "{\"ok\":true,\"ver\":\"" FW_VERSION "\",\"port\":8080}");
  }
  else if (head.startsWith("POST /ota"))
  {
    handlePost(c, head);
  }
  // ---- 网页控制台 API（与蓝牙控制台共用 otaApiDispatch）----
  else if (head.startsWith("GET /api/") || head.startsWith("POST /api/"))
  {
    bool isGet = head.startsWith("GET");
    int sp1 = head.indexOf(' ');
    int sp2 = head.indexOf(' ', sp1 + 1);
    String path = head.substring(sp1 + 1, sp2);

    String body;
    if (!isGet)
    {
      int cl = headerInt(head, "Content-Length");
      if (cl < 0) cl = 0;   // 未带 Content-Length 视为空 body（如 /api/dbg?on=1 的开关调用）
      if (cl > 0 && !readBody(c, cl, body)) { reply(c, 400, "{\"err\":\"body 不完整\"}"); return; }
    }

    int code;
    String out;
    bool isText = false;
    if (otaApiDispatch(isGet ? "GET" : "POST", path, headerStr(head, "X-Pin"),
                       body, false, code, out, isText))
    {
      if (isText) replyText(c, out.c_str());
      else reply(c, code, out.c_str());
    }
    else
      reply(c, 400, "{\"err\":\"unknown path\"}");
  }
  else
  {
    reply(c, 400, "{\"err\":\"unknown path\"}");
  }
}

static void httpTask(void *)
{
  for (;;)
  {
    bool up = (WiFi.status() == WL_CONNECTED);
    if (up && !bound)
    {
      srv->begin();
      srv->setNoDelay(true);
      bound = true;
      logTeePrintf("[http] ota server http://%s:%u/ota\n",
                    WiFi.localIP().toString().c_str(), (unsigned)OTA_HTTP_PORT);
      // ArduinoOTA 在 WiFi 连上之前就调过 MDNS.begin()，那时可能没网卡而失败；
      // 这里补一次，保证 esp32-ai-s3.local 可解析（同名，重复调用是幂等的）
      static bool mdnsTried = false;
      if (!mdnsTried)
      {
        mdnsTried = true;
        if (MDNS.begin("esp32-ai-s3"))
        {
          MDNS.addService("http", "tcp", OTA_HTTP_PORT);
          logTeePrintf("[mdns] http://esp32-ai-s3.local:%u/ota\n", (unsigned)OTA_HTTP_PORT);
        }
        else
        {
          logTeePrintf("[mdns] begin failed（用 IP 访问即可）\n");
        }
      }
    }
    else if (!up && bound)
    {
      srv->end();
      bound = false;
    }

    if (bound)
    {
      WiFiClient c = srv->accept();
      if (c)
      {
        handleClient(c);
        c.stop();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void otaHttpBegin()
{
  if (srv) return;
  srv = new WiFiServer(OTA_HTTP_PORT);
  srv->begin();
  BaseType_t tok = xTaskCreate(httpTask, "otahttp", 8192, nullptr, 2, nullptr);
  if (tok != pdPASS)
    logTeePrintf("[http] task create FAILED（内部内存不足）\n");
  else
    logTeePrintf("[http] task started, port %u\n", (unsigned)OTA_HTTP_PORT);
}

bool otaHttpWriting() { return writing; }

String otaHttpUrl()
{
  if (WiFi.status() != WL_CONNECTED) return String();
  return "http://" + WiFi.localIP().toString() + ":" + String(OTA_HTTP_PORT) + "/ota";
}

void otaHttpPoll()
{
  String u = otaHttpUrl();
  if (u.length() && u != lastInfo)
  {
    lastInfo = u;
    otaBleSetInfo(u);   // 网页连上蓝牙后读这个特征，自动填 IP
  }
}
