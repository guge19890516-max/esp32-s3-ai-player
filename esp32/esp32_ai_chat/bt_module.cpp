#include "bt_module.h"
#include "cfg_table.h"
#include "log_tee.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// BT04E 会话走 UART2，空闲时 end() 释放引脚；HTTP 任务与屏幕主循环都可能进来，用互斥量串行化
static HardwareSerial btSerial(2);
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t sMtx = nullptr;

static void ensureMtx()
{
  if (sMtx) return;
  portENTER_CRITICAL(&sMux);
  if (!sMtx) sMtx = xSemaphoreCreateMutex();
  portEXIT_CRITICAL(&sMux);
}

// —— AT 口调试态（定义在会话函数之前，sessionOpen 要查它）——
static bool dbgOn = false;
static std::vector<uint8_t> dbgAcc;   // 调试模式收到的原始字节（网页轮询取走）
static uint32_t dbgTotal = 0;

// 从 UART 收已到的字节进缓冲（须持锁且调试口已开）
static void dbgPumpLocked()
{
  while (btSerial.available())
  {
    uint8_t b = (uint8_t)btSerial.read();
    dbgAcc.push_back(b);
    if (dbgAcc.size() > 8192)
      dbgAcc.erase(dbgAcc.begin(), dbgAcc.begin() + 2048);   // 防膨胀：丢最老
    dbgTotal++;
  }
}

static int cfgIntKey(const char *key, int def)
{
  int i = cfgIndexOf(key);
  if (i < 0) return def;
  return (int)cfgGetRaw(cfgAt(i)).toInt();
}

// 按当前配置打开 UART（须已持锁）。AT 口 TX/RX 是 ESP32 侧的引脚：TX 接模块 RX、RX 接模块 TX
static bool sessionOpen()
{
  if (dbgOn)
  {
    logTeePrintf("[btmod] AT 会话被调试口占用，请先关闭调试口\n");
    return false;
  }
  int tx = cfgIntKey("btmod.tx", 41);
  int rx = cfgIntKey("btmod.rx", 42);
  long baud = cfgIntKey("btmod.baud", 9600);
  if (baud < 1200 || baud > 921600) baud = 9600;
  if (tx <= 0 || tx > 48 || rx <= 0 || rx > 48 || tx == rx)
  {
    logTeePrintf("[btmod] 引脚非法 tx=%d rx=%d（请在配置项里修改）\n", tx, rx);
    return false;
  }
  btSerial.begin(baud, SERIAL_8N1, rx, tx);
  btSerial.setTimeout(5);
  uint32_t t0 = millis();   // 清掉上电残留输入
  while (millis() - t0 < 30)
  {
    while (btSerial.available()) btSerial.read();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

static void sessionClose()
{
  btSerial.flush();   // 等 TX 发完再关
  btSerial.end();
}

// 发送并收集响应（须已持锁、会话已开）。BT04E 响应以 OK / ERROR=码 结束
static String sendCmd(const String &cmd, uint32_t timeoutMs)
{
  while (btSerial.available()) btSerial.read();   // 丢弃上一条的尾巴
  String out = cmd;
  out += "\r\n";
  btSerial.print(out);
  logTeePrintf("[btmod] > %s\\r\\n\n", cmd.c_str());

  String resp;
  resp.reserve(128);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs)
  {
    while (btSerial.available() && resp.length() < 512)
      resp += (char)btSerial.read();
    if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0)
    {
      vTaskDelay(pdMS_TO_TICKS(5));   // 稍等尾部字节一起收
      while (btSerial.available() && resp.length() < 512)
        resp += (char)btSerial.read();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  String show = resp;
  show.trim();
  if (show.length()) logTeePrintf("[btmod] < %s\n", show.c_str());
  else logTeePrintf("[btmod] < (无响应)\n");
  return resp;
}

String btmodCmd(const String &cmd, uint32_t timeoutMs)
{
  ensureMtx();
  if (xSemaphoreTake(sMtx, pdMS_TO_TICKS(300)) != pdTRUE)
    return "ERROR=busy";
  String resp;
  if (sessionOpen())
    resp = sendCmd(cmd, timeoutMs);
  else
    resp = "ERROR=pin";
  sessionClose();
  xSemaphoreGive(sMtx);
  return resp;
}

bool btmodSet(const String &cmd)
{
  return btmodCmd(cmd).indexOf("OK") >= 0;
}

// 从响应里取 "+KEY=值" 那一段的值；找不到返回空
static String respField(const String &resp, const char *key)
{
  String pat = String("+") + key + "=";
  int p = resp.indexOf(pat);
  if (p < 0) return String();
  int s = p + pat.length();
  int e = s;
  while (e < (int)resp.length() && resp[e] != '\r' && resp[e] != '\n') e++;
  String v = resp.substring(s, e);
  v.trim();
  return v;
}

String btmodInfo()
{
  ensureMtx();
  if (xSemaphoreTake(sMtx, pdMS_TO_TICKS(300)) != pdTRUE)
    return "{\"err\":\"busy\"}";
  String ver, mac, name, baud, pari, stop;
  if (sessionOpen())
  {
    logTeePrintf("[btmod] info：AT 命令仅在模块无蓝牙连接时有效\n");
    ver  = respField(sendCmd("AT+VERSION", 700), "VERSION");
    mac  = respField(sendCmd("AT+LADDR", 700), "LADDR");
    name = respField(sendCmd("AT+NAME", 700), "NAME");
    baud = respField(sendCmd("AT+BAUD", 700), "BAUD");
    pari = respField(sendCmd("AT+PARI", 700), "PARI");
    stop = respField(sendCmd("AT+STOP", 700), "STOP");
  }
  sessionClose();
  xSemaphoreGive(sMtx);

  // 波特率代码 → 实际值（1=2400 ... 7=115200），解析不了就原样透出
  static const long BAUDS[7] = {2400, 4800, 9600, 19200, 38400, 57600, 115200};
  String baudTxt = baud;
  if (baud.length() == 1 && baud[0] >= '1' && baud[0] <= '7')
    baudTxt = String(BAUDS[baud[0] - '1']);
  String pariTxt = (pari == "0") ? "无校验" : (pari == "1") ? "奇校验" : (pari == "2") ? "偶校验" : pari;
  String stopTxt = (stop == "0") ? "1位" : (stop == "1") ? "2位" : stop;

  String out = "{\"version\":\"" + cfgJsonEsc(ver) + "\"";
  out += ",\"mac\":\"" + cfgJsonEsc(mac) + "\"";
  out += ",\"name\":\"" + cfgJsonEsc(name) + "\"";
  out += ",\"baud\":\"" + cfgJsonEsc(baudTxt) + "\",\"baudCode\":\"" + cfgJsonEsc(baud) + "\"";
  out += ",\"pari\":\"" + cfgJsonEsc(pariTxt) + "\",\"pariCode\":\"" + cfgJsonEsc(pari) + "\"";
  out += ",\"stop\":\"" + cfgJsonEsc(stopTxt) + "\",\"stopCode\":\"" + cfgJsonEsc(stop) + "\"";
  out += "}";
  return out;
}

// 设置类命令（改参数要 AT+RESET 才生效）——成功后由 API 层补发 AT+RESET
bool btmodNeedsReset(const String &cmd)
{
  static const char *PFX[] = {
      "AT+BAUD", "AT+PARI", "AT+STOP", "AT+NAME", "AT+UUID", "AT+CHAR",
      "AT+WRITE", "AT+TEADV", "AT+SEADV", "AT+NOTI", "AT+ADVI", "AT+ADTY",
      "AT+OPASS", "AT+APASS", "AT+TYPE", "AT+MODE", "AT+PIN", "AT+DEFAULT"};
  String c = cmd;
  c.trim();
  c.toUpperCase();
  for (unsigned i = 0; i < sizeof(PFX) / sizeof(PFX[0]); i++)
    if (c.startsWith(PFX[i])) return true;
  return false;
}

// —— AT 口原始收发调试：GPIO41/42 变透传口，配合网页控制台验证串口链路本身 ——

bool btmodDebug(bool on)
{
  ensureMtx();
  if (xSemaphoreTake(sMtx, pdMS_TO_TICKS(300)) != pdTRUE) return false;
  bool ok = true;
  if (on && !dbgOn)
  {
    dbgAcc.clear();
    dbgTotal = 0;
    ok = sessionOpen();   // 引脚/波特率取配置项；此时 dbgOn 还没置位，可正常开
    dbgOn = ok;
    logTeePrintf("[btmod] 调试口 %s（tx=%d rx=%d @ %ld，期间 AT 会话不可用）\n",
                 ok ? "已打开" : "打开失败",
                 cfgIntKey("btmod.tx", 41), cfgIntKey("btmod.rx", 42),
                 cfgIntKey("btmod.baud", 9600));
  }
  else if (!on && dbgOn)
  {
    dbgOn = false;
    sessionClose();
    logTeePrintf("[btmod] 调试口已关闭，本次共收 %lu 字节\n", (unsigned long)dbgTotal);
  }
  xSemaphoreGive(sMtx);
  return ok;
}

bool btmodDebugOn()
{
  return dbgOn;
}

size_t btmodDebugSend(const String &data, bool hexMode, String &err)
{
  ensureMtx();
  if (xSemaphoreTake(sMtx, pdMS_TO_TICKS(300)) != pdTRUE) { err = "忙"; return 0; }
  size_t n = 0;
  if (!dbgOn)
    err = "调试口未打开";
  else if (hexMode)
  {
    String clean;   // 只留十六进制字符
    for (unsigned i = 0; i < data.length(); i++)
    {
      char c = data[i];
      if (isxdigit((unsigned char)c)) clean += c;
      else if (c != ' ' && c != ',' && c != ':') { err = "含非十六进制字符"; break; }
    }
    if (!err.length() && (clean.length() % 2)) err = "HEX 长度须为偶数";
    if (!err.length())
      for (unsigned i = 0; i + 1 < clean.length(); i += 2)
      {
        uint8_t b = (uint8_t)strtol(clean.substring(i, i + 2).c_str(), nullptr, 16);
        btSerial.write(&b, 1);
        n++;
      }
    if (n) btSerial.flush();
  }
  else
  {
    n = data.length();
    btSerial.print(data);
    btSerial.flush();
  }
  if (n) dbgPumpLocked();   // 顺带收对面刚回的字节
  xSemaphoreGive(sMtx);
  if (n) logTeePrintf("[btmod] 调试发送 %u 字节\n", (unsigned)n);
  return n;
}

String btmodDebugRecvHex()
{
  ensureMtx();
  if (xSemaphoreTake(sMtx, pdMS_TO_TICKS(300)) != pdTRUE) return String();
  if (dbgOn) dbgPumpLocked();
  String out;
  char tmp[4];
  for (unsigned i = 0; i < dbgAcc.size(); i++)
  {
    snprintf(tmp, sizeof(tmp), "%02X", dbgAcc[i]);
    out += tmp;
    if (i + 1 < dbgAcc.size()) out += ' ';
  }
  dbgAcc.clear();
  xSemaphoreGive(sMtx);
  return out;
}

uint32_t btmodDebugRxCount()
{
  return dbgTotal;
}

// 主循环轻量收取：抢不到锁就跳过，下轮再试，避免高频收发把硬件 FIFO 撑爆
void btmodDebugPump()
{
  if (!dbgOn) return;
  if (xSemaphoreTake(sMtx, 0) != pdTRUE) return;
  if (dbgOn) dbgPumpLocked();
  xSemaphoreGive(sMtx);
}
