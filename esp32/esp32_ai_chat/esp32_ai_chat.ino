#include <WiFi.h>
#include <algorithm>
#include "board_config.h"
#include "ui.h"
#include "secrets.h"
#include "touch_driver.h"
#include "config_store.h"
#include "chat_store.h"
#include "keyboard.h"
#include "ai_client.h"
#include "frame_log.h"
#include "text_codec.h"
#include "serial_link.h"
#include "ota_update.h"
#include "ota_http.h"
#include "bms_client.h"
#include "cfg_table.h"
#include "bt_module.h"
#include "ble_relay.h"
#include "rec_store.h"
#include "log_store.h"
#include "rule_engine.h"
#include "app_ctrl.h"
#include "sim_gen.h"

enum Screen : uint8_t { S_HOME, S_CHAT, S_SETTINGS, S_WIFI, S_WIFIPASS, S_API, S_XIAOZHI, S_SERIAL, S_FRAMES, S_AIANA, S_OTA, S_OTAURL, S_BMS, S_STATUS, S_CONF, S_BTMOD, S_RELAY, S_MODELS, S_DECODE, S_SIMIO, S_SIMTE };
static Screen scr = S_CHAT;

static bool dirtyFull = true;
static bool dirtyChat = false;
static bool dirtyInput = false;

static int chatScroll = 0;
static String draft;

static int chatViewBot()
{
  return kbIsHidden() ? SCREEN_H - INPUT_H : CHAT_BOT;
}

static String toastMsg;
static uint32_t toastUntil = 0;
static bool needRestore = false;

static bool dlgClear = false;
static bool pickOpen = false;   // 枚举选择弹窗打开中（定义见 pickOpenDlg 区块）

struct ApInfo { String ssid; int rssi; };
static std::vector<ApInfo> apList;
static bool scanPending = false;
static int scanRetry = 0;
static int wifiScroll = 0;

static String selSsid;
static bool wcConnecting = false;
static uint32_t wcStart = 0;
static String wcSsid, wcPw;
static String passDraft;   // WiFi 密码独立缓冲，不再借用聊天草稿

static int editField = -1;
static String editDraft;

// 配置项页（S_CONF）：两级结构——先分类列表，点进组显示该组配置项
static int confScroll = 0;
static int confIdx = -1;      // >=0 表示正在键盘编辑第 confIdx 项
static int confGroup = -1;    // -1=分类列表，>=0=正在显示的分组
static String confDraft;

// 分组信息：TABLE 内同组行连续排列
static int confGroupCount()
{
  int n = 0;
  const char *cur = nullptr;
  for (int i = 0; i < cfgCount(); i++)
  {
    if (!cur || strcmp(cfgAt(i).group, cur) != 0) { cur = cfgAt(i).group; n++; }
  }
  return n;
}

static int confGroupStart(int g)   // 组 g 的第一个项索引
{
  int n = -1;
  const char *cur = nullptr;
  for (int i = 0; i < cfgCount(); i++)
  {
    if (!cur || strcmp(cfgAt(i).group, cur) != 0) { cur = cfgAt(i).group; n++; }
    if (n == g) return i;
  }
  return 0;
}

static int confGroupLen(int g)
{
  int s = confGroupStart(g);
  int e = (g + 1 < confGroupCount()) ? confGroupStart(g + 1) : cfgCount();
  return e - s;
}

// 触摸抬起保护：按下时发生了跳页/退出编辑，就把同一次触摸的 release 吞掉，
// 防止它落进新页面的列表被当成点选（配置项页“进页就弹编辑”的根因）
static bool releaseGuard = false;

// WiFi 密码页：本次进入是否来自已记住的网络（明文预填密码，可直接连接）
static bool wifiPassSaved = false;

// 模型列表页（S_MODELS）
static int modelsScroll = 0;

static int modelsCount()
{
  String ms = cfgModels();
  if (!ms.length()) return 0;
  int n = 1;
  for (int i = 0; i < ms.length(); i++)
    if (ms[i] == '\n') n++;
  return n;
}

static String modelsAt(int i)
{
  String ms = cfgModels();
  int start = 0;
  for (int k = 0; k <= i; k++)
  {
    int p = ms.indexOf('\n', start);
    if (k == i)
      return (p < 0) ? ms.substring(start) : ms.substring(start, p);
    start = p + 1;
  }
  return String();
}

static bool lastDown = false;
static int lastX = 0, lastY = 0, downX = 0, downY = 0;
static int movedDist = 0;
static bool chatDragging = false;
static bool listDragging = false;
static int dragStartY = 0, scrollAtDragStart = 0;

static uint8_t lastBusyPhase = 0xFF;
static uint8_t lastBlink = 0;
static uint32_t lastWatchdog = 0;

static const int TERM_TOP = 162;
static const int ANS_TOP = 126;
static const int ANS_BOT = 426;

static std::vector<String> termLines;
static std::vector<uint8_t> termTx;
static int serialScroll = 0;
static bool serialEol = true;
static size_t lastFrameCount = 0;
static String lastSlStatus;

static int framesScroll = 0;
static int anaIdx = -1;
static int anaScroll = 0;
static bool anaWorking = false;
static String anaErr;
static std::vector<String> anaLines;
static uint8_t aiOwner = 0;
static uint8_t lastOtaPhase = 0xFF;
static uint8_t lastOtaState = 0xFF;
static int lastOtaPct = -1;

static String otaUrl;
static String otaUrlEdit;   // 地址编辑缓冲（取消时丢弃）

static const char *AI_SYS_ANALYZE = "你是一位嵌入式通信协议分析专家，精通 BLE、串口、AT 指令和各类 IoT 设备协议解析。用简洁准确的中文回答，尽量给出字节级别的解释，不要编造不存在的协议。";

static const int LIST_TOP = 76;
static const int ITEM_H = 44;

static void gotoScreen(Screen s)
{
  scr = s;
  pickOpen = false;   // 跳页时关闭可能残留的选项弹窗
  dirtyFull = true;
  chatDragging = false;
  listDragging = false;
}

static void showToast(const String &s, uint32_t ms = 2000)
{
  toastMsg = s;
  toastUntil = millis() + ms;
  needRestore = true;
}

static bool wifiUp()
{
  return WiFi.status() == WL_CONNECTED;
}

static void wrapLines(const String &s, int maxW, std::vector<String> &out)
{
  out.clear();
  String line;
  int w = 0;
  int i = 0;
  while (i < s.length())
  {
    uint8_t c = (uint8_t)s[i];
    int cl = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    if (i + cl > s.length()) cl = s.length() - i;
    int cw = (cl == 1) ? 9 : 17;
    if (w + cw > maxW && line.length())
    {
      out.push_back(line);
      line = "";
      w = 0;
    }
    line += s.substring(i, i + cl);
    w += cw;
    i += cl;
  }
  out.push_back(line);
}

static void buildTermLines()
{
  termLines.clear();
  termTx.clear();
  for (auto &f : frames)
  {
    String head = String(f.rx ? "RX " : "TX ") + flTime(f.ms) + " " + String(f.len) + "B ";
    String body = flHexMode ? f.hex : f.text;
    if (!body.length()) body = flHexMode ? "(空)" : "(无可打印内容)";
    std::vector<String> ls;
    wrapLines(head + body, 296, ls);
    for (auto &l : ls)
    {
      termLines.push_back(l);
      termTx.push_back(f.rx ? 0 : 1);
    }
  }
}

static int hexToBytes(const String &s, uint8_t *out, int maxLen)
{
  int n = 0;
  int hi = -1;
  for (int i = 0; i < s.length() && n < maxLen; i++)
  {
    int c = (uint8_t)s[i];
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == ' ' || c == ',') continue;
    else return -1;
    if (hi < 0) hi = v;
    else
    {
      out[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  return (hi < 0) ? n : -1;
}

static void serialSend()
{
  String s = kbPeek();
  s.trim();
  if (!s.length()) { showToast("请输入要发送的内容"); return; }

  // 发送目标（配置项 relay.tx）：手机+模块 / 仅手机 / 仅模块
  int ti = cfgIndexOf("relay.tx");
  String tgt = ti >= 0 ? cfgGetRaw(cfgAt(ti)) : String("手机+模块");
  bool toPhone = tgt != "仅模块";
  bool toModule = tgt != "仅手机";
  if (toPhone && !slActive())
  {
    showToast(slMode() == LINK_BLE ? "BLE 尚未连接" : "TCP 尚未连接");
    return;
  }
  if (toModule && !relayConnected()) { showToast("中继未连接模块"); return; }

  if (flHexMode)
  {
    uint8_t buf[256];
    int n = hexToBytes(s, buf, sizeof(buf));
    if (n <= 0) { showToast("HEX 格式错误，例如 AA 55 01"); return; }
    String bin;
    bin.reserve(n);
    bin.concat((const char *)buf, n);
    if (toPhone && !slSend(bin)) { showToast("发送失败"); return; }
    if (toModule) relaySend(buf, n);   // 经模块上 485 总线
    kbTake();
    flAddTxRaw(buf, n);
  }
  else
  {
    String out = s;
    if (serialEol) out += "\r\n";
    String wire = tcEncodeTx(out);   // 按「发送编码」转成链路字节（UTF-8 时原样）
    if (toPhone && !slSend(wire)) { showToast("发送失败"); return; }
    if (toModule) relaySend((const uint8_t *)wire.c_str(), wire.length());
    kbTake();
    flAddTxWire(s, (const uint8_t *)wire.c_str(), wire.length());   // hex=实际字节 text=原输入
  }
  dirtyInput = true;
  dirtyFull = true;
}

static void startAnalyze(int idx)
{
  if (idx < 0 || idx >= (int)frames.size()) { showToast("报文已过期"); return; }
  if (!wifiUp()) { showToast("未联网，请先在设置中配置 WiFi"); return; }
  if (otaBusy()) { showToast("固件升级中，请稍候"); return; }
  if (aiBusy()) { showToast("AI 正在回复，请稍候"); return; }
  FrameRec f = frames[idx];
  String p = "请分析这条串口报文。\n";
  p += String("方向: ") + (f.rx ? "设备接收" : "设备发送") + "\n";
  p += "时间: " + flTime(f.ms) + "\n";
  p += "长度: " + String(f.len) + " 字节\n";
  p += "十六进制: " + f.hex + "\n";
  p += "可打印文本: " + (f.text.length() ? f.text : String("无")) + "\n\n";
  p += "请用中文回答: 1) 这可能是哪个协议或哪条指令; 2) 逐字节拆解每个字段的含义; 3) 有无异常或风险; 4) 如果需要回复该指令，建议发送什么内容。";
  std::vector<AiMsg> hist;
  hist.push_back({"user", p});
  anaIdx = idx;
  anaScroll = 0;
  anaErr = "";
  anaLines.clear();
  anaWorking = true;
  aiOwner = 1;
  aiStartEx(hist, apiEndpoint(cfg.url), cfg.key, cfg.model, AI_SYS_ANALYZE);
  gotoScreen(S_AIANA);
}

static void startWifiConnect(const String &ssid, const String &pw)
{
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.setAutoReconnect(true);   // 扫描时被临时关掉，连网时恢复
  wcSsid = ssid;
  wcPw = pw;
  WiFi.begin(ssid.c_str(), pw.c_str());
  wcConnecting = true;
  wcStart = millis();
}

static void serviceWifiConnect()
{
  if (!wcConnecting) return;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED)
  {
    wcConnecting = false;
    cfg.ssid = wcSsid;
    cfg.wpass = wcPw;
    cfgSaveWiFi();
    wifiRecUpsert(wcSsid, wcPw);   // 像手机一样记住连过的 WiFi，下次免输入
    kbReset();
    kbAttach(&draft);
    kbSetMask(false);
    kbSetSendLabel("发送");
    showToast("已连接 " + WiFi.localIP().toString());
    gotoScreen(S_SETTINGS);
    return;
  }
  if (millis() - wcStart > 15000 || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)
  {
    wcConnecting = false;
    showToast("连接失败，请检查密码");
    dirtyFull = true;
  }
}

static void scanStart()
{
  apList.clear();
  scanRetry = 0;
  // 家里连不上已保存的网络时，STA 一直处于连接/自动重连状态占住射频，
  // 导致 esp_wifi_scan_start 返回 ESP_ERR_WIFI_STATE（WIFI_SCAN_FAILED，秒回“未发现网络”）。
  // 扫描前先停掉自动重连并断开，让 STA 空闲下来（已保存的 cfg.ssid 不受影响）。
  if (!wifiUp())
  {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(150);
  }
  WiFi.scanNetworks(true, true);
  scanPending = true;
  dirtyFull = true;
}

static void serviceScan()
{
  if (!scanPending) return;
  int n = WiFi.scanComplete();
  if (n >= 0)
  {
    apList.clear();
    for (int i = 0; i < n; i++)
    {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      ApInfo a{s, WiFi.RSSI(i)};
      apList.push_back(a);
    }
    sort(apList.begin(), apList.end(), [](const ApInfo &a, const ApInfo &b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
    scanPending = false;
    wifiScroll = 0;
    dirtyFull = true;
  }
  else if (n == WIFI_SCAN_FAILED)
  {
    WiFi.scanDelete();
    if (scanRetry < 5)
    {
      scanRetry++;
      delay(150);
      WiFi.scanNetworks(true, true);
    }
    else
    {
      scanPending = false;
      showToast("扫描失败，请重试");
      dirtyFull = true;
    }
  }
}

#define AI_HIST_CHARS 6000   // 单次请求携带的历史正文字数上限

static void sendToAi()
{
  String text = kbPeek();
  text.trim();
  if (!text.length()) return;
  if (!wifiUp()) { showToast("未联网，请先在设置中配置 WiFi"); return; }
  if (otaBusy()) { showToast("固件升级中，请稍候"); return; }
  if (aiBusy()) { showToast("AI 正在回复，请稍候"); return; }
  kbTake();
  chatAdd(true, text);
  chatScroll = 0;
  dirtyChat = true;
  dirtyInput = true;
  // 历史按总字数从最近往回裁剪，避免一次请求把堆与上行带宽吃满
  std::vector<AiMsg> hist;
  int budget = AI_HIST_CHARS;
  for (int i = (int)msgs.size() - 1; i >= 0; i--)
  {
    ChatMsg &m = msgs[i];
    if (m.err) continue;
    if (!hist.empty() && (int)m.text.length() > budget) break;
    budget -= (int)m.text.length();
    AiMsg am;
    am.role = m.user ? "user" : "assistant";
    am.content = m.text;
    hist.insert(hist.begin(), am);
    if ((int)hist.size() >= 20 || budget <= 0) break;
  }
  aiStart(hist, apiEndpoint(cfg.url), cfg.key, cfg.model);
  logTeePrintf("[ui] send hist=%u url=%s\n", (unsigned)hist.size(), cfg.url.c_str());   // 崩溃锚点：发送动作已递交 AI 任务
}

static void serviceAi()
{
  String rep, err;
  if (!aiPoll(rep, err)) return;
  if (aiOwner == 1)
  {
    anaWorking = false;
    if (err.length()) anaErr = err;
    else
    {
      wrapLines(rep, 296, anaLines);
      anaScroll = 0;
    }
    if (scr == S_AIANA) dirtyFull = true;
  }
  else
  {
    if (err.length()) chatAdd(false, "[错误] " + err, true);
    else chatAdd(false, rep);
    chatScroll = 0;
    dirtyChat = true;
  }
  aiOwner = 0;
}

static void drawSplash(const String &msg)
{
  bootSplash(msg);
}

static void drawTitleBar()
{
  gfx->fillRect(0, 0, SCREEN_W, TITLE_H, COL_PANEL);
  gfx->drawFastHLine(0, TITLE_H, SCREEN_W, COL_LINE);
  if (scr == S_CHAT)
  {
    drawTextAt(12, centerBaseY(0, TITLE_H), "AI 聊天", COL_TEXT, COL_PANEL);
    bool up = wifiUp();
    gfx->fillCircle(236, TITLE_H / 2, 5, up ? COL_OK : 0xF980);
    drawBtn(254, 4, 62, 28, "主页", COL_KEY_FN, COL_TEXT);
  }
  else if (scr == S_HOME)
  {
    drawTextAt(12, centerBaseY(0, TITLE_H), "应用", COL_TEXT, COL_PANEL);
    bool up = wifiUp();
    gfx->fillCircle(236, TITLE_H / 2, 5, up ? COL_OK : 0xF980);
  }
  else
  {
    drawBtn(3, 4, 56, 28, "主页", COL_KEY_FN, COL_TEXT);
const char *t = (scr == S_SETTINGS) ? "设置" : (scr == S_WIFI) ? "WiFi 设置"
                                     : (scr == S_WIFIPASS)        ? "输入密码"
                                     : (scr == S_XIAOZHI)         ? "小志"
                                     : (scr == S_SERIAL)          ? "串口助手"
                                     : (scr == S_FRAMES)          ? "报文记录"
                                     : (scr == S_AIANA)           ? "AI 报文分析"
                                     : (scr == S_OTA)             ? "固件升级"
                                     : (scr == S_OTAURL)          ? "升级地址"
                                     : (scr == S_CONF)            ? "配置项"
                                     : (scr == S_BTMOD)           ? "蓝牙模块"
                                     : (scr == S_RELAY)           ? "蓝牙中继"
                                     : (scr == S_MODELS)          ? "模型列表"
                                     : (scr == S_STATUS)          ? "设备状态"
                                     : (scr == S_DECODE)          ? "报文解码"
                                                                   : "API 设置";
    drawTextAt(70, centerBaseY(0, TITLE_H), t, COL_TEXT, COL_PANEL);
  }
}

static void drawSettingsBody()
{
  // 12 行：行高 32、步进 36，最后一行 436..468，全部在 480 屏内
  struct Row { int y; const char *label; };
  Row rows[12] = {
    {40, "WiFi 设置"},
    {76, "AI 接口设置"},
    {112, "蓝牙模块"},
    {148, "蓝牙中继"},
    {184, "报文解码"},
    {220, "电池监测 (BMS)"},
    {256, "固件升级 (OTA)"},
    {292, "设备状态"},
    {328, "配置项"},
    {364, "清除聊天记录"},
    {400, "重启设备"},
    {436, "返回主页"},
  };
  for (auto &r : rows)
  {
    gfx->fillRoundRect(3, r.y, 314, 32, 8, COL_PANEL);
    drawTextAt(14, centerBaseY(r.y, 32), r.label, COL_TEXT, COL_PANEL);
    drawTextAt(296, centerBaseY(r.y, 32), ">", COL_SUB, COL_PANEL);
  }
}

static void drawProgress(int x, int y, int w, int h, int p)
{
  gfx->drawRoundRect(x, y, w, h, h / 2, COL_LINE);
  int fw = (int)((long)(w - 4) * p / 100);
  if (fw > 0)
  {
    gfx->fillRoundRect(x + 2, y + 2, fw, h - 4, (h - 4) / 2, COL_ACCENT);
  }
}

static void drawOtaBody()
{
  gfx->fillRoundRect(3, 42, 314, 96, 8, COL_PANEL);
  drawTextAt(12, 62, "固件版本", COL_SUB, COL_PANEL);
  drawTextAt(96, 62, fitTail(otaVersion(), 212), COL_TEXT, COL_PANEL);
  drawTextAt(12, 86, "运行槽位", COL_SUB, COL_PANEL);
  drawTextAt(96, 86, otaSlot(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 110, "空闲分区", COL_SUB, COL_PANEL);
  drawTextAt(96, 110, otaFreeSpace(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 130, fitTail("空闲内存 " + String(ESP.getFreeHeap() / 1024) + " KB", 290), COL_SUB, COL_PANEL);

  OtaState s = otaState();
  gfx->fillRoundRect(3, 146, 314, 96, 8, COL_PANEL);
  drawTextAt(12, 166, "网络推送 OTA", COL_TEXT, COL_PANEL);
  String hint = otaArmed() ? "已开启，电脑同网内可推送" : "已关闭";
  drawTextAt(240, 166, otaArmed() ? "开启" : "关闭", otaArmed() ? COL_OK : COL_SUB, COL_PANEL);
  String m = otaMessage().length() ? otaMessage() : (s == OST_IDLE ? hint : String("空闲"));
  if (s == OST_IDLE) m = hint;
  uint16_t c = (s == OST_FAIL) ? COL_ERR_TX : (s == OST_DONE ? COL_OK : (s == OST_RUNNING ? COL_ACCENT : COL_SUB));
  drawTextAt(12, 194, fitTail(m, 296), c, COL_PANEL);
  if (s == OST_RUNNING)
  {
    drawProgress(12, 206, 296, 14, otaPercent());
    drawTextAt(12, 236, String(otaPercent()) + "%", COL_SUB, COL_PANEL);
  }

  gfx->fillRoundRect(3, 250, 314, 78, 8, COL_PANEL);
  drawTextAt(12, 270, "URL 固件升级", COL_TEXT, COL_PANEL);
  drawTextAt(12, 294, fitTail(otaUrl.length() ? otaUrl : String("例 http://192.168.1.5:8000/firmware.bin"), 296),
             otaUrl.length() ? COL_TEXT : COL_SUB, COL_PANEL);
  drawTextAt(12, 318, fitTail("电脑开 HTTP 服务器提供 firmware.bin", 296), COL_SUB, COL_PANEL);

  drawBtn(3, 338, 154, 44, otaArmed() ? "关闭推送" : "开启推送", otaArmed() ? COL_KEY_FN : COL_ACCENT, 0xFFFF);
  drawBtn(163, 338, 154, 44, "设置地址", COL_KEY_FN, COL_TEXT);
  drawBtn(3, 390, 314, 44, "开始 URL 升级", COL_ACCENT, 0xFFFF);
  drawTextAt(14, 448, otaBleReady() ? "蓝牙升级：服务就绪，用 ble_ota 网页" : "蓝牙升级：服务未注册！无法网页升级",
             otaBleReady() ? COL_SUB : COL_ERR_TX, COL_BG);
  drawTextAt(14, 466, "SD 升级：/update/fw.bin 开机自动烧录", COL_SUB, COL_BG);
}

static void drawOtaUrlBody()
{
  drawTextAt(14, 58, "填写 firmware.bin 的下载地址", COL_SUB, COL_BG);
  gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
  String v = otaUrlEdit;
  v += ((millis() / 500) & 1) ? "_" : "";
  drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
  drawTextAt(14, 136, "本机 IP: " + slLocalIp(), COL_SUB, COL_BG);
  drawTextAt(14, 160, "电脑上执行 python -m http.server 8000", COL_SUB, COL_BG);
  drawTextAt(14, 184, "再把电脑 IP 和端口填到上面", COL_SUB, COL_BG);
  drawTextAt(14, 208, "返回=放弃修改，保存=写入 NVS", COL_SUB, COL_BG);
  drawBtn(3, 246, 100, 44, "返回", COL_KEY_FN, COL_TEXT);
  drawBtn(110, 246, 100, 44, "保存", COL_ACCENT, 0xFFFF);
  drawBtn(217, 246, 100, 44, "保存并升级", COL_ACCENT, 0xFFFF);
}

// 设置 → 设备状态：升级进度 + 蓝牙连接信息 + 网络/内存 + 网页 PIN
static void drawStatusBody()
{
  gfx->fillRoundRect(3, 42, 314, 96, 8, COL_PANEL);
  drawTextAt(12, 58, "固件", COL_SUB, COL_PANEL);
  drawTextAt(70, 58, fitTail(otaVersion(), 236), COL_TEXT, COL_PANEL);
  drawTextAt(12, 76, "槽位", COL_SUB, COL_PANEL);
  drawTextAt(70, 76, otaSlot(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 94, "目标分区", COL_SUB, COL_PANEL);
  drawTextAt(70, 94, otaFreeSpace(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 112, "网页 PIN", COL_SUB, COL_PANEL);
  if (cfgWebPinRequired())
  {
    char b[8];
    snprintf(b, sizeof(b), "%04u", (unsigned)cfgPin());
    drawTextAt(70, 112, String("已开启 ") + b, COL_ACCENT, COL_PANEL);
  }
  else
  {
    drawTextAt(70, 112, "未启用（配置项 → 网页需要 PIN）", COL_SUB, COL_PANEL);
  }

  gfx->fillRoundRect(3, 146, 314, 88, 8, COL_PANEL);
  OtaState s = otaState();
  String st;
  uint16_t sc = COL_SUB;
  if (otaBleActive())
  {
    st = "正在传输固件 " + String(otaPercent()) + "%";
    sc = COL_ACCENT;
  }
  else if (s == OST_RUNNING)
  {
    st = "正在写入固件 " + String(otaPercent()) + "%";
    sc = COL_ACCENT;
  }
  else if (s == OST_DONE)
  {
    st = "升级完成";
    sc = COL_OK;
  }
  else if (s == OST_FAIL)
  {
    st = "升级失败";
    sc = COL_ERR_TX;
  }
  else st = "空闲";
  drawTextAt(12, 164, st, sc, COL_PANEL);
  drawProgress(12, 174, 296, 14, otaPercent());
  String m = (s == OST_IDLE && !otaBleActive()) ? String("当前没有升级任务") : otaMessage();
  drawTextAt(12, 216, fitTail(m, 296), COL_SUB, COL_PANEL);

  gfx->fillRoundRect(3, 242, 314, 104, 8, COL_PANEL);
  drawTextAt(12, 260, "蓝牙名称", COL_SUB, COL_PANEL);
  drawTextAt(92, 260, slDeviceName(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 278, "连接状态", COL_SUB, COL_PANEL);
  drawTextAt(92, 278, slBleConnected() ? "已连接" : "未连接", slBleConnected() ? COL_OK : COL_SUB, COL_PANEL);
  drawTextAt(12, 296, "广播状态", COL_SUB, COL_PANEL);
  drawTextAt(92, 296, slBleAdvActive() ? "广播中" : "未广播", slBleAdvActive() ? COL_OK : COL_ERR_TX, COL_PANEL);
  drawTextAt(12, 314, "本机地址", COL_SUB, COL_PANEL);
  drawTextAt(92, 314, slBleAddr(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 332, "网页升级", COL_SUB, COL_PANEL);
  drawTextAt(92, 332, otaBleReady() ? "服务就绪" : "服务未注册", otaBleReady() ? COL_OK : COL_ERR_TX, COL_PANEL);

  gfx->fillRoundRect(3, 354, 314, 120, 8, COL_PANEL);
  bool up = wifiUp();
  drawTextAt(12, 372, "WiFi", COL_SUB, COL_PANEL);
  drawTextAt(92, 372, up ? WiFi.SSID() : String("未连接"), up ? COL_OK : COL_SUB, COL_PANEL);
  drawTextAt(12, 390, "本机 IP", COL_SUB, COL_PANEL);
  drawTextAt(92, 390, up ? WiFi.localIP().toString() : String("-"), COL_TEXT, COL_PANEL);
  drawTextAt(12, 408, "网页升级", COL_SUB, COL_PANEL);
  if (up)
    drawTextAt(92, 408, "端口 " + String(OTA_HTTP_PORT) + " 已就绪", COL_OK, COL_PANEL);
  else
    drawTextAt(92, 408, "需要先连上 WiFi", COL_ERR_TX, COL_PANEL);
  drawTextAt(12, 426, "信号", COL_SUB, COL_PANEL);
  drawTextAt(92, 426, up ? String(WiFi.RSSI()) + " dBm" : String("-"), COL_TEXT, COL_PANEL);
  drawTextAt(12, 444, "空闲内存", COL_SUB, COL_PANEL);
  drawTextAt(92, 444, String(ESP.getFreeHeap() / 1024) + " KB / 最大块 " +
                            String(ESP.getMaxAllocHeap() / 1024) + " KB", COL_TEXT, COL_PANEL);
}

// 设置 → 配置项：列表由 cfg_table 生成，网页端同源；点按编辑
// （BOOL 直接翻转 / ENUM 循环 / STR 与 INT 走键盘编辑）
static void drawConfBody()
{
  if (confIdx >= 0)
  {
    const CfgItem &it = cfgAt(confIdx);
    drawTextAt(14, 58, it.label, COL_SUB, COL_BG);
    gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
    String v = (it.flags & CFG_SECRET)
                   ? [] { String s; int n = utf8Count(confDraft); for (int i = 0; i < n; i++) s += '*'; return s; }()
                   : confDraft;
    v += ((millis() / 500) & 1) ? "_" : "";
    drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
    String tip = (it.type == CT_INT) ? (String("范围 ") + it.minv + " ~ " + it.maxv)
                 : (it.type == CT_ENUM) ? (String("可选: ") + it.options)
                                        : String("输入后点保存，返回=取消");
    drawTextAt(14, 136, tip, COL_SUB, COL_BG);
    drawBtn(3, 246, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
    drawBtn(217, 246, 100, 44, "保存", COL_ACCENT, 0xFFFF);
    return;
  }

  // 两级列表：confGroup<0 显示分类，>=0 显示该组的配置项
  int rows = (confGroup < 0) ? confGroupCount() : confGroupLen(confGroup);
  int contentH = rows * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (confScroll > maxScroll) confScroll = maxScroll;
  if (confScroll < 0) confScroll = 0;

  if (confGroup < 0)
  {
    for (int g = 0; g < confGroupCount(); g++)
    {
      int iy = LIST_TOP + g * ITEM_H - confScroll;
      if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
      int gs = confGroupStart(g);
      gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
      drawTextAt(14, iy + 15, cfgAt(gs).group, COL_TEXT, COL_PANEL);
      drawTextAt(14, iy + 32, String(confGroupLen(g)) + " 项 >", COL_SUB, COL_PANEL);
    }
  }
  else
  {
    int gs = confGroupStart(confGroup);
    for (int k = 0; k < confGroupLen(confGroup); k++)
    {
      int i = gs + k;
      int iy = LIST_TOP + k * ITEM_H - confScroll;
      if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
      const CfgItem &it = cfgAt(i);
      gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
      drawTextAt(14, iy + 15, it.label, COL_TEXT, COL_PANEL);
      String t = (it.type == CT_STR) ? "文本" : (it.type == CT_INT) ? "数字"
                                   : (it.type == CT_BOOL)           ? "开关"
                                   : (it.type == CT_PAGE)           ? "进入"
                                                                    : "选择";
      drawTextAt(232, iy + 15, t, COL_SUB, COL_PANEL);
      String v = cfgGetShow(it);
      if (it.type == CT_BOOL || it.type == CT_PAGE)
      {
        bool on = (v == "1");
        if (it.type == CT_PAGE)
          v = on ? "发帧中 >" : "已停止 >";
        else
          v = on ? "开" : "关";
        drawTextAt(14, iy + 32, fitTail(v, 296), on ? COL_OK : COL_SUB, COL_PANEL);
        continue;
      }
      if (!v.length()) v = "未设置";
      drawTextAt(14, iy + 32, fitTail(v, 296), COL_SUB, COL_PANEL);
    }
  }
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, confScroll);
}

static void confLeaveEdit(bool save)
{
  releaseGuard = true;   // 取消/保存按钮的抬起不许落进下面的列表
  if (save && confIdx >= 0)
  {
    const CfgItem &it = cfgAt(confIdx);
    String err;
    if (!cfgSet(it, confDraft, err))
    {
      showToast(err, 2600);   // 校验失败留在编辑页，用户可改
      dirtyFull = true;
      return;
    }
    showToast("已保存");
    if (!strcmp(it.key, "net.ssid") || !strcmp(it.key, "net.pass"))
      showToast("已保存，重新连接 WiFi 后生效", 2600);
  }
  confIdx = -1;
  kbReset();
  kbAttach(&draft);
  kbSetMask(false);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

// ENUM 选项循环逻辑已由 pickOpenDlg 弹窗取代（点条目弹出全部选项直接选）

// —— 枚举选择弹窗：点条目弹出全部选项直接选，不再点击循环切换 ——
static int pickMode = 0;          // 1=写配置项 2=AT+BAUD 3=AT+PARI 4=AT+STOP
static String pickTitle;
static std::vector<String> pickOpts;
static int pickCur = -1;          // 当前值高亮行，-1=无
static CfgItem pickItem;          // pickMode==1 时要写的配置项
static std::vector<String> btmodLines;   // 信息/应答展示区（已折行）；提前声明供弹窗应用逻辑使用
static void btmodApply(const String &cmd);   // 前置声明：弹窗应用时要发 AT

static void pickOpenDlg(int mode, const String &title, const String &opts,
                        const String &cur, const CfgItem &item = CfgItem())
{
  pickMode = mode;
  pickTitle = title;
  pickOpts.clear();
  int i = 0;
  while (i <= opts.length())
  {
    int p = opts.indexOf('|', i);
    if (p < 0) p = opts.length();
    String o = opts.substring(i, p);
    o.trim();
    if (o.length()) pickOpts.push_back(o);
    i = p + 1;
  }
  pickCur = -1;
  for (unsigned k = 0; k < pickOpts.size(); k++)
    if (pickOpts[k] == cur) { pickCur = k; break; }
  pickItem = item;
  pickOpen = true;
  dirtyFull = true;
}

static void pickApply(int idx)
{
  if (idx < 0 || idx >= (int)pickOpts.size()) return;
  String v = pickOpts[idx];
  String err;
  if (pickMode == 1)
  {
    if (cfgSet(pickItem, v, err)) showToast("已设为 " + v);
    else showToast(err, 2600);
  }
  else if (pickMode == 2)   // 波特率：AT+BAUD1~7，成功后同步配置项
  {
    btmodApply(String("AT+BAUD") + String(idx + 1));
    if (btmodLines.size())
    {
      int bi = cfgIndexOf("btmod.baud");
      if (bi >= 0) { cfgSet(cfgAt(bi), v, err); showToast("AT 口波特率已设为 " + v, 2400); }
    }
  }
  else if (pickMode == 3)   // 校验位：0无 1奇 2偶
  {
    btmodApply(String("AT+PARI") + String(idx));
    if (btmodLines.size()) showToast("校验位已设为 " + v, 2200);
  }
  else if (pickMode == 4)   // 停止位：1位/2位
  {
    btmodApply(String("AT+STOP") + String(idx));
    if (btmodLines.size()) showToast("停止位已设为 " + v, 2200);
  }
  pickOpen = false;
  dirtyFull = true;
}

static void pickClose()
{
  pickOpen = false;
  dirtyFull = true;
}

static void handlePickTouch(bool press, int x, int y)
{
  if (!press) return;
  const int RH = 34, TW = 280;
  int n = (int)pickOpts.size();
  int h = 40 + n * RH + 10;
  int top = (SCREEN_H - h) / 2;
  if (x >= 30 && x <= 20 + TW - 10 && y >= top + 40 && y < top + 40 + n * RH)
  {
    int k = (y - top - 40) / RH;
    if (k >= 0 && k < n) { pickApply(k); return; }
  }
  pickClose();   // 点弹窗外 = 取消
}

static void drawPickDialog()
{
  const int RH = 34, TW = 280;
  int n = (int)pickOpts.size();
  if (!n) { pickOpen = false; return; }
  int h = 40 + n * RH + 10;
  int top = (SCREEN_H - h) / 2;
  gfx->fillRoundRect(20, top, TW, h, 12, COL_PANEL);
  gfx->drawRoundRect(20, top, TW, h, 12, COL_ACCENT);
  drawTextAt(32, top + 26, pickTitle, COL_SUB, COL_PANEL);
  for (int k = 0; k < n; k++)
  {
    int ry = top + 40 + k * RH;
    bool cur = (k == pickCur);
    gfx->fillRoundRect(30, ry, TW - 20, RH - 6, 6, cur ? COL_ACCENT : COL_PANEL);
    drawTextAt(42, ry + 20, pickOpts[k] + (cur ? String("  · 当前") : String()), cur ? 0xFFFF : COL_TEXT,
               cur ? COL_ACCENT : COL_PANEL);
  }
}

static void handleConfTouch(bool press, bool release, bool down, int x, int y)
{
  if (pickOpen) { handlePickTouch(press, x, y); return; }
  if (confIdx >= 0)
  {
    if (press)
    {
      if (ptIn(x, y, 3, 4, 56, 28)) { confLeaveEdit(false); return; }
      if (ptIn(x, y, 3, 246, 100, 44)) { confLeaveEdit(false); return; }
      if (ptIn(x, y, 217, 246, 100, 44)) { confLeaveEdit(true); return; }
    }
    uint8_t ev = kbHandle(down, x, y);
    if (ev & KB_CHANGED) dirtyFull = true;
    if (ev & KB_SEND) confLeaveEdit(true);
    return;
  }

  // 左上角返回：组内返回分类列表，分类列表返回设置页
  if (press && ptIn(x, y, 3, 4, 56, 28))
  {
    if (confGroup >= 0) { confGroup = -1; confScroll = 0; dirtyFull = true; }
    else gotoScreen(S_SETTINGS);
    return;
  }

  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = confScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int rows = (confGroup < 0) ? confGroupCount() : confGroupLen(confGroup);
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, rows * ITEM_H - viewH);
      confScroll = scrollAtDragStart - dy;
      if (confScroll < 0) confScroll = 0;
      if (confScroll > maxScroll) confScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;

  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    if (confGroup < 0)
    {
      int g = (y + confScroll - LIST_TOP) / ITEM_H;
      if (g >= 0 && g < confGroupCount())
      {
        confGroup = g;
        confScroll = 0;
        dirtyFull = true;
      }
      return;
    }

    int k = (y + confScroll - LIST_TOP) / ITEM_H;
    if (k >= 0 && k < confGroupLen(confGroup))
    {
      int idx = confGroupStart(confGroup) + k;
      const CfgItem &it = cfgAt(idx);
      if (it.type == CT_PAGE)
      {
        gotoScreen(!strcmp(it.key, "sim.io") ? S_SIMIO : S_SIMTE);
        return;
      }
      if (it.type == CT_BOOL)
      {
        String cur = cfgGetRaw(it);
        String err;
        if (cfgSet(it, cur == "1" ? "0" : "1", err))
          showToast(cfgGetRaw(it) == "1" ? "已开启" : "已关闭");
        else showToast(err, 2600);
        dirtyFull = true;
      }
      else if (it.type == CT_ENUM)
      {
        pickOpenDlg(1, String("选择 ") + it.label, it.options, cfgGetRaw(it), it);
      }
      else
      {
        confIdx = idx;
        kbAttach(&confDraft);
        kbSetMask((it.flags & CFG_SECRET) != 0);
        kbSetSendLabel("保存");
        kbReset();
        confDraft = cfgGetRaw(it);   // kbReset 会清空绑定缓冲，之后重新赋值
        dirtyFull = true;
      }
    }
  }
}


// —— 蓝牙模块页（S_BTMOD）：BT04E 的 AT 配置（查询/波特率/校验/停止位/名称/重启/恢复出厂）——
// AT 收发是阻塞式的（模块应答很快，正常 <200ms）；发之前先画一帧“执行中”再阻塞，避免像死机
static int btmodScroll = 0;
static String btmodStatus = "就绪";       // 底部一行状态提示
static bool btmodNameEdit = false;        // 正在用键盘输入模块名称
static String btmodNameDraft;
static uint32_t btmodConfirmUntil = 0;    // 恢复出厂的二次确认截止时刻

static const int BTMOD_TOP = 42;          // 信息面板
static const int BTMOD_BOT = 192;
static const char *BTMOD_BAUDS[7] = {"2400", "4800", "9600", "19200", "38400", "57600", "115200"};   // 对应 AT+BAUD1~7

static void compose();   // 前置声明：AT 执行前先画一帧“执行中”

static void btmodShow(const String &text)
{
  btmodLines.clear();
  String t = text.length() ? text : String("(无响应)");
  // 模块应答常带 \r\n，错误波特率下还会混入二进制；控制符会让 print() 回车/叠字，统一清洗
  String clean;
  clean.reserve(t.length() + 8);
  for (unsigned i = 0; i < t.length(); i++)
  {
    char c = t[i];
    if (c == '\n' || (uint8_t)c >= 0x20) clean += c;   // 丢弃 \r 及其他控制符
  }
  wrapLines(clean, 296, btmodLines);
  btmodScroll = 0;
}

// 从响应里取 “+KEY=值” 的值（解析不到返回空）
static String btmodField(const String &resp, const char *key)
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

// 从 btmodInfo 的 JSON 里取字符串字段（解析不到返回空）
static String btmodJsonStr(const String &j, const char *key)
{
  String pat = String("\"") + key + "\":\"";
  int p = j.indexOf(pat);
  if (p < 0) return String();
  int s = p + pat.length();
  int e = s;
  while (e < (int)j.length() && j[e] != '"') e++;
  return j.substring(s, e);
}

// 发一条 AT 命令并展示过程/应答
static String btmodExec(const String &cmd)
{
  btmodStatus = "执行中 " + cmd + " ...";
  dirtyFull = true;
  compose();
  String r = btmodCmd(cmd, 900);
  bool ok = r.indexOf("OK") >= 0;
  r.trim();
  btmodStatus = String(ok ? "已发送 " : "无应答 ") + cmd;
  btmodShow(cmd + " →\n" + (r.length() ? r : String("(无响应)")));
  dirtyFull = true;
  return r;
}

// 设置类命令：发命令 + 自动补 AT+RESET（BT04E 改参数后必须重启才生效）
static void btmodApply(const String &cmd)
{
  String r = btmodExec(cmd);
  if (r.indexOf("OK") < 0)
  {
    showToast("模块未确认，请检查接线/波特率", 2600);
    return;
  }
  btmodExec("AT+RESET");
  showToast("已写入，模块已重启", 1800);
}

// 查询模块信息（走 btmodInfo 一次会话查 6 项，再拼成人话）
static void btmodQuery()
{
  btmodStatus = "正在查询模块...";
  dirtyFull = true;
  compose();
  String j = btmodInfo();
  btmodStatus = "查询完成";
  String ver = btmodJsonStr(j, "version"), mac = btmodJsonStr(j, "mac"), nm = btmodJsonStr(j, "name");
  String bd = btmodJsonStr(j, "baud"), pa = btmodJsonStr(j, "pari"), st = btmodJsonStr(j, "stop");
  String text = "版本: " + (ver.length() ? ver : String("未应答"));
  text += "\nMAC: " + (mac.length() ? mac : String("未应答"));
  text += "\n名称: " + (nm.length() ? nm : String("未应答"));
  text += "\n波特率: " + (bd.length() ? bd : String("未应答"));
  text += "\n校验位: " + (pa.length() ? pa : String("未应答"));
  text += "\n停止位: " + (st.length() ? st : String("未应答"));
  btmodShow(text);
  dirtyFull = true;
}

// 测试连接：只发一条 AT，按应答给出排错提示（电脑串口工具能通而 ESP32 不通时，多半是接线/波特率/被蓝牙占用）
static void btmodTest()
{
  btmodStatus = "测试中：发送 AT ...";
  dirtyFull = true;
  compose();
  String r = btmodCmd("AT", 600);
  r.trim();
  bool ok = r.indexOf("OK") >= 0;
  if (ok)
  {
    btmodStatus = "连接正常，模块应答 OK";
    btmodShow("AT → " + r);
  }
  else
  {
    btmodStatus = "无应答，按提示排查";
    String tip = "AT → " + (r.length() ? r : String("(无响应)"));
    tip += "\n\n排查顺序：\n1. 模块正被手机/电脑蓝牙连着（连着不响应 AT）";
    tip += "\n2. RX/TX 是否交叉：模块TXD→GPIO42，RXD→GPIO41";
    tip += "\n3. AT 口波特率与模块是否一致（配置项 btmod.baud，默认9600）";
    tip += "\n4. 是否共地（GND）";
    btmodShow(tip);
  }
  dirtyFull = true;
  showToast(ok ? "连接正常" : "无应答", 1500);
}

// 设波特率/校验位/停止位：弹出选项列表直接选（原循环切换已废弃）
static void btmodPickBaud()
{
  int i = cfgIndexOf("btmod.baud");
  if (i < 0) return;
  String opts;
  for (int k = 0; k < 7; k++)
  {
    if (k) opts += "|";
    opts += BTMOD_BAUDS[k];
  }
  pickOpenDlg(2, "选择波特率", opts, cfgGetRaw(cfgAt(i)));
}

static void btmodPickPari()
{
  String cur = btmodField(btmodExec("AT+PARI"), "PARI");
  String c = (cur == "1") ? "奇校验" : (cur == "2") ? "偶校验" : "无校验";
  pickOpenDlg(3, "选择校验位", "无校验|奇校验|偶校验", c);
}

static void btmodPickStop()
{
  String cur = btmodField(btmodExec("AT+STOP"), "STOP");
  pickOpenDlg(4, "选择停止位", "1位|2位", (cur == "2") ? "2位" : "1位");
}

static void btmodNameLeave(bool save)
{
  releaseGuard = true;   // 取消/保存按钮的抬起不许落进下面的按钮区
  if (save)
  {
    String nm = btmodNameDraft;
    nm.trim();
    if (!nm.length() || nm.length() > 28)
    {
      showToast("名称需 1~28 字节", 2400);   // 校验失败留在编辑态，用户可改
      dirtyFull = true;
      return;
    }
    btmodApply(String("AT+NAME") + nm);
  }
  btmodNameEdit = false;
  kbReset();
  kbAttach(&draft);
  kbSetMask(false);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

// 恢复出厂：3 秒内按两次才执行；AT 口波特率配置同步回 9600
static void btmodFactory()
{
  if (millis() > btmodConfirmUntil)
  {
    btmodConfirmUntil = millis() + 3000;
    showToast("再按一次确认恢复出厂", 2400);
    dirtyFull = true;
    return;
  }
  btmodConfirmUntil = 0;
  btmodExec("AT+DEFAULT");
  btmodExec("AT+RESET");
  int i = cfgIndexOf("btmod.baud");
  if (i >= 0) { String err; cfgSet(cfgAt(i), "9600", err); }
  showToast("已恢复出厂（AT 口波特率回 9600）", 2600);
}

static void drawBtmodBody()
{
  if (btmodNameEdit)
  {
    drawTextAt(14, 58, "改模块名称", COL_SUB, COL_BG);
    gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
    String v = btmodNameDraft;
    v += ((millis() / 500) & 1) ? "_" : "";
    drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
    drawTextAt(14, 136, "输入后点保存，返回=取消（≤28 字节）", COL_SUB, COL_BG);
    drawBtn(3, 246, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
    drawBtn(217, 246, 100, 44, "保存", COL_ACCENT, 0xFFFF);
    return;
  }

  // 信息/应答面板（可拖动滚动）
  gfx->fillRoundRect(3, BTMOD_TOP, 314, BTMOD_BOT - BTMOD_TOP, 8, COL_PANEL);
  drawTextAt(12, BTMOD_TOP + 18, "模块信息 / AT 应答", COL_SUB, COL_PANEL);
  if (!btmodLines.size())
    drawTextAt(12, BTMOD_TOP + 44, "点「查模块信息」读取版本/MAC/名称等", COL_SUB, COL_PANEL);
  else
  {
    int viewH = BTMOD_BOT - BTMOD_TOP - 34;
    int maxScroll = max(0, (int)btmodLines.size() * LINE_H - viewH);
    if (btmodScroll > maxScroll) btmodScroll = maxScroll;
    if (btmodScroll < 0) btmodScroll = 0;
    int i0 = max(0, (btmodScroll + LINE_H - 1) / LINE_H);
    for (int i = i0; i < (int)btmodLines.size(); i++)
    {
      int ly = BTMOD_TOP + 30 + i * LINE_H - btmodScroll;
      if (ly + LINE_H > BTMOD_BOT - 4) break;
      drawTextAt(10, ly + 17, btmodLines[i], COL_TEXT, COL_PANEL);
    }
    drawScrollbar(SCREEN_W - 5, BTMOD_TOP + 30, viewH - 6, (int)btmodLines.size() * LINE_H, btmodScroll);
  }

  // 2×4 操作区（测试连接是排错第一步；恢复出厂半宽带二次确认）
  drawBtn(3, 198, 154, 40, "测试连接", COL_ACCENT, 0xFFFF);
  drawBtn(163, 198, 154, 40, "查模块信息", COL_KEY_FN, COL_TEXT);
  drawBtn(3, 242, 154, 40, "设波特率", COL_KEY_FN, COL_TEXT);
  drawBtn(163, 242, 154, 40, "设校验位", COL_KEY_FN, COL_TEXT);
  drawBtn(3, 286, 154, 40, "设停止位", COL_KEY_FN, COL_TEXT);
  drawBtn(163, 286, 154, 40, "改模块名称", COL_KEY_FN, COL_TEXT);
  drawBtn(3, 330, 154, 40, "重启模块", COL_KEY_FN, COL_TEXT);
  drawBtn(163, 330, 154, 40, (millis() < btmodConfirmUntil) ? "再按一次确认！" : "恢复出厂",
          COL_ERR_BG, COL_ERR_TX);

  drawTextAt(12, 424, btmodStatus, COL_SUB, COL_BG);
  drawTextAt(12, 444, "AT 命令仅在模块无蓝牙连接时有效", COL_SUB, COL_BG);
  drawTextAt(12, 464, "接线 TXD→42 / RXD→41，波特率见配置项", COL_SUB, COL_BG);
}

static void handleBtmodTouch(bool press, bool release, bool down, int x, int y)
{
  if (pickOpen) { handlePickTouch(press, x, y); return; }
  if (btmodNameEdit)
  {
    if (press)
    {
      if (ptIn(x, y, 3, 4, 56, 28) || ptIn(x, y, 3, 246, 100, 44)) { btmodNameLeave(false); return; }
      if (ptIn(x, y, 217, 246, 100, 44)) { btmodNameLeave(true); return; }
    }
    uint8_t ev = kbHandle(down, x, y);
    if (ev & KB_CHANGED) dirtyFull = true;
    if (ev & KB_SEND) btmodNameLeave(true);
    return;
  }

  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { releaseGuard = true; gotoScreen(S_SETTINGS); return; }
    if (ptIn(x, y, 3, 198, 154, 40)) { btmodTest(); return; }
    if (ptIn(x, y, 163, 198, 154, 40)) { btmodQuery(); return; }
    if (ptIn(x, y, 3, 242, 154, 40)) { btmodPickBaud(); return; }
    if (ptIn(x, y, 163, 242, 154, 40)) { btmodPickPari(); return; }
    if (ptIn(x, y, 3, 286, 154, 40)) { btmodPickStop(); return; }
    if (ptIn(x, y, 163, 286, 154, 40))
    {
      btmodNameEdit = true;
      kbAttach(&btmodNameDraft);
      kbSetMask(false);
      kbSetSendLabel("保存");
      kbReset();
      dirtyFull = true;
      return;
    }
    if (ptIn(x, y, 3, 330, 154, 40)) { btmodExec("AT+RESET"); showToast("已发 AT+RESET", 1600); return; }
    if (ptIn(x, y, 163, 330, 154, 40)) { btmodFactory(); return; }
  }

  // 信息面板拖动滚动
  if (down && !press && y >= BTMOD_TOP && y < BTMOD_BOT)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = btmodScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = BTMOD_BOT - BTMOD_TOP - 34;
      int maxScroll = max(0, (int)btmodLines.size() * LINE_H - viewH);
      btmodScroll = scrollAtDragStart - dy;
      if (btmodScroll < 0) btmodScroll = 0;
      if (btmodScroll > maxScroll) btmodScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
}

static void drawBmsBody()
{
  gfx->fillRoundRect(3, 42, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 62, "状态", COL_SUB, COL_PANEL);
  drawTextAt(96, 62, bms::connected() ? (bms::hasData() ? "已连接" : "连接中..") : "扫描中..",
              bms::hasData() ? COL_OK : COL_ACCENT, COL_PANEL);

  if (!bms::hasData())
  {
    // fix14：可见扫描列表——周围所有 BLE 广播都能看到，点选尝试连接
    int n = bms::scanCount();
    if (bms::connecting())
      drawTextAt(12, 118, "正在连接（最多 8 秒），列表暂停更新…", COL_ACCENT, COL_BG);
    else
      drawTextAt(12, 118, String("正在扫描周围蓝牙设备，已发现 ") + n + " 台：",
                 n ? COL_TEXT : COL_SUB, COL_BG);
    int rowY = 140;
    int shown = 0;
    for (int i = 0; i < n && shown < 5; i++)
    {
      bms::ScanItem it = bms::scanAt(i);
      String label = it.name.length() ? it.name : ("未命名 " + String(it.addr.toString().c_str()));
      if (label.length() > 18) label = label.substring(0, 17) + "..";
      gfx->fillRoundRect(3, rowY, 314, 38, 6, COL_PANEL);
      drawTextAt(12, rowY + 11, label, it.hasFF00 ? COL_OK : COL_TEXT, COL_PANEL);
      String tail = String(it.rssi) + "dBm" + (it.hasFF00 ? " [BMS]" : "");
      drawTextAt(314 - 12 - tail.length() * 12, rowY + 11, tail, COL_SUB, COL_PANEL);
      rowY += 44;
      shown++;
    }
    if (!n)
      drawTextAt(12, rowY + 6, "列表为空：电池需上电/按键唤醒后才广播", COL_SUB, COL_BG);
    // fix14：扫描+返回双按钮，出页路径不能丢
    drawBtn(3, 440, 154, 40, "重新扫描", COL_KEY_FN, COL_TEXT);
    drawBtn(163, 440, 154, 40, "返回", COL_KEY_FN, COL_TEXT);
    return;
  }

  bms::BaseInfo I = bms::info();
  gfx->fillRoundRect(3, 104, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 124, "总压", COL_SUB, COL_PANEL);
  drawTextAt(96, 124, String(I.totalVoltage, 2) + " V", COL_TEXT, COL_PANEL);
  drawTextAt(12, 146, "电流", COL_SUB, COL_PANEL);
  drawTextAt(96, 146, String(I.current, 2) + " A", COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, 162, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 182, "SOC", COL_SUB, COL_PANEL);
  drawTextAt(96, 182, String(I.soc) + " %", COL_TEXT, COL_PANEL);
  drawTextAt(12, 204, "循环", COL_SUB, COL_PANEL);
  drawTextAt(96, 204, String(I.cycles), COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, 220, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 240, "剩余容量", COL_SUB, COL_PANEL);
  drawTextAt(96, 240, String(I.remainPower, 2) + " Ah", COL_TEXT, COL_PANEL);
  drawTextAt(12, 262, "标称容量", COL_SUB, COL_PANEL);
  drawTextAt(96, 262, String(I.nominalPower, 2) + " Ah", COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, 278, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 298, "温度1", COL_SUB, COL_PANEL);
  drawTextAt(96, 298, String(I.temperatures[0], 1) + " C", COL_TEXT, COL_PANEL);
  drawTextAt(12, 320, "温度2", COL_SUB, COL_PANEL);
  drawTextAt(96, 320, String(I.temperatures[1], 1) + " C", COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, 336, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 356, "电芯数", COL_SUB, COL_PANEL);
  drawTextAt(96, 356, String(I.cellCount), COL_TEXT, COL_PANEL);
  drawTextAt(12, 378, "平衡电流", COL_SUB, COL_PANEL);
  drawTextAt(96, 378, String(I.balanceCurrent, 2) + " A", COL_TEXT, COL_PANEL);

  // 返回按钮
  drawBtn(3, 440, 314, 40, "返回", COL_KEY_FN, COL_TEXT);
}

// —— 蓝牙中继页（S_RELAY）：主机连接蓝牙串口模块（BT04E），报文进「串口助手」并转发手机 ——
static void drawRelayBody()
{
  bool on = relayEnabled();
  gfx->fillRoundRect(3, 42, 314, 52, 8, COL_PANEL);
  drawTextAt(12, 52, "模块连接", COL_SUB, COL_PANEL);
  String st;
  uint16_t sc = COL_SUB;
  if (relayConnected()) { st = "已连接"; sc = COL_OK; }
  else if (relayConnecting()) { st = "连接中.."; sc = COL_ACCENT; }
  else if (relayScanActive()) { st = "扫描中.."; sc = COL_ACCENT; }
  else st = "未连接";
  drawTextAt(96, 52, st, sc, COL_PANEL);
  drawTextAt(12, 76, "报文见「串口助手」页，并同步转发手机", COL_SUB, COL_PANEL);

  drawBtn(3, 104, 154, 40, on ? "关闭中继" : "开启中继", on ? COL_ERR_BG : COL_OK, on ? COL_ERR_TX : 0xFFFF);
  drawBtn(163, 104, 154, 40, "重新扫描", COL_KEY_FN, COL_TEXT);

  if (relayConnected())
  {
    gfx->fillRoundRect(3, 156, 314, 100, 8, COL_PANEL);
    drawTextAt(12, 176, "设备", COL_SUB, COL_PANEL);
    drawTextAt(96, 176, fitTail(relayConnName(), 212), COL_TEXT, COL_PANEL);
    bool p = relayPassOn();
    drawTextAt(12, 200, "手机→模块透传", COL_SUB, COL_PANEL);
    drawTextAt(296, 200, p ? "开" : "关", p ? COL_OK : COL_SUB, COL_PANEL);
    int ti = cfgIndexOf("relay.tx");
    String tgt = ti >= 0 ? cfgGetRaw(cfgAt(ti)) : String("手机+模块");
    drawTextAt(12, 224, "发送目标 " + tgt + "（配置项可改）", COL_SUB, COL_PANEL);
    drawBtn(3, 268, 102, 40, "查看报文", COL_KEY_FN, COL_TEXT);
    drawBtn(109, 268, 102, 40, "解码", COL_ACCENT, 0xFFFF);
    drawBtn(215, 268, 102, 40, "断开连接", COL_ERR_BG, COL_ERR_TX);
    drawBtn(3, 440, 314, 40, "返回", COL_KEY_FN, COL_TEXT);
    return;
  }

  // 未连接：可见扫描列表（同 BMS 页），点选连接
  int n = relayScanCount();
  if (relayConnecting())
    drawTextAt(12, 164, "正在连接（最多 8 秒）…", COL_ACCENT, COL_BG);
  else
    drawTextAt(12, 164, String("周围蓝牙设备 ") + n + " 台，点选连接：", n ? COL_TEXT : COL_SUB, COL_BG);
  int rowY = 186;
  int shown = 0;
  for (int i = 0; i < n && shown < 4; i++)
  {
    ScanItem it = relayScanAt(i);
    String label = it.name.length() ? it.name : ("未命名 " + String(it.addr.toString().c_str()));
    if (label.length() > 18) label = label.substring(0, 17) + "..";
    gfx->fillRoundRect(3, rowY, 314, 38, 6, COL_PANEL);
    drawTextAt(12, rowY + 11, label, it.match ? COL_OK : COL_TEXT, COL_PANEL);
    String tail = String(it.rssi) + "dBm" + (it.match ? " [目标]" : "");
    drawTextAt(314 - 12 - tail.length() * 12, rowY + 11, tail, COL_SUB, COL_PANEL);
    rowY += 44;
    shown++;
  }
  if (!n && !relayScanActive())
    drawTextAt(12, rowY + 6, "开启中继后将自动扫描目标模块", COL_SUB, COL_BG);
  drawBtn(3, 440, 314, 40, "返回", COL_KEY_FN, COL_TEXT);
}

// —— 报文解码页（S_DECODE）：上半当前值按设备分块，下半解码历史（BMS 页同风格）——
static std::vector<String> decLines;   // 解码历史（最新在尾部，最多 40 条）
static int decScroll = 0;              // 历史上卷像素
static int decDevSel = 0;              // 选中的设备下标（多设备规则）

static void decPushHistory(const String &s)
{
  if (decLines.size() > 39) decLines.erase(decLines.begin());
  decLines.push_back(s);
  decScroll = 0;
}

// ===== 模拟开关量页（S_SIMIO）：路按钮网格，点击切换并与位图 hex 双向同步 =====

static int simIoCnt()
{
  int i = cfgIndexOf("sim.cnt");
  return constrain(i >= 0 ? cfgGetRaw(cfgAt(i)).toInt() : 8, 1, 64);
}

static String simIoMode()
{
  int i = cfgIndexOf("sim.mode");
  return i >= 0 ? cfgGetRaw(cfgAt(i)) : String("随机");
}

static void simIoMaskSave()
{
  // 当前位图按路数转大端 hex 写回配置（与帧内数据区一致，"01"=路1开）
  int nb = (simIoCnt() + 7) / 8;
  uint64_t st = sim::bits();
  char hex[17];
  for (int i = 0; i < nb; i++)
    snprintf(hex + i * 2, 3, "%02X", (unsigned)((st >> (8 * (nb - 1 - i))) & 0xFF));
  String err;
  int i = cfgIndexOf("sim.mask");
  if (i >= 0) cfgSet(cfgAt(i), String(hex), err);
}

static void drawSimIoBody()
{
  bool run = sim::on();
  drawTextAt(14, centerBaseY(4, 28), "← 模拟开关量", COL_TEXT, COL_BG);
  String st = run ? "发帧中" : "已停止";
  drawTextAt(240, centerBaseY(4, 28), st, run ? COL_OK : COL_SUB, COL_BG);
  drawTextAt(12, 70, String("共 ") + simIoCnt() + " 路 · " + simIoMode() + "模式 · 点按钮切换开/关", COL_SUB, COL_BG);

  // 路按钮网格：8 列 x 8 行，64 路一屏放完
  int cnt = simIoCnt();
  uint64_t bits = sim::bits();
  for (int idx = 0; idx < cnt; idx++)
  {
    int r = idx / 8, c = idx % 8;
    int x = 6 + c * 38, y = 84 + r * 43;
    bool on = (bits >> idx) & 1ULL;
    gfx->fillRect(x, y, 35, 40, on ? COL_OK : COL_PANEL_HI);
    char lb[8];
    snprintf(lb, sizeof(lb), "%d", idx + 1);
    drawTextAt(x + 17 - textWidth(lb) / 2, centerBaseY(y, 40), lb, on ? 0x0000 : COL_SUB, on ? COL_OK : COL_PANEL_HI);
  }

  drawBtn(6, 436, 96, 40, run ? "发帧:开" : "发帧:关", run ? COL_OK : COL_KEY_FN, run ? 0xFFFF : COL_TEXT);
  drawBtn(112, 436, 96, 40, simIoMode() == "固定" ? "固定" : "随机", COL_KEY_FN, COL_TEXT);
  drawBtn(218, 436, 96, 40, "返回", COL_KEY_FN, COL_TEXT);
}

static void handleSimIoTouch(bool press, bool release, bool down, int x, int y)
{
  (void)down;
  if (press && ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_CONF); return; }

  if (press && y >= 84 && y < 428)
  {
    int c = (x - 6) / 38, r = (y - 84) / 43;
    if (c >= 0 && c < 8 && r >= 0 && r < 8)
    {
      int idx = r * 8 + c;
      int bx = 6 + c * 38, by = 84 + r * 43;
      if (idx < simIoCnt() && x >= bx && x < bx + 35 && y >= by && y < by + 40)
      {
        sim::setBits(sim::bits() ^ (1ULL << idx));
        simIoMaskSave();
        dirtyFull = true;
      }
    }
    return;
  }

  if (press && y >= 436)
  {
    String err;
    if (ptIn(x, y, 6, 436, 96, 40))
    {
      int i = cfgIndexOf("sim.io");
      if (i >= 0) cfgSet(cfgAt(i), sim::on() ? "0" : "1", err);   // cfgSet 内同步内存开关
      dirtyFull = true;
    }
    else if (ptIn(x, y, 112, 436, 96, 40))
    {
      int i = cfgIndexOf("sim.mode");
      if (i >= 0) cfgSet(cfgAt(i), simIoMode() == "固定" ? "随机" : "固定", err);
      dirtyFull = true;
    }
    else if (ptIn(x, y, 218, 436, 96, 40))
      gotoScreen(S_CONF);
  }
}

// ===== 模拟遥测页（S_SIMTE）：通道数 +/-，每通道基准值 +/-（步进 10） =====

static int simTeCnt()
{
  int i = cfgIndexOf("te.cnt");
  return constrain(i >= 0 ? cfgGetRaw(cfgAt(i)).toInt() : 4, 1, 8);
}

static long simTeBase(int ch)   // ch: 1~8
{
  char k[8];
  snprintf(k, sizeof(k), "te.ch%d", ch);
  int i = cfgIndexOf(k);
  return constrain(i >= 0 ? (long)cfgGetRaw(cfgAt(i)).toInt() : 0L, 0L, 65535L);
}

static void simTeSet(const char *key, long v)
{
  int i = cfgIndexOf(key);
  if (i < 0) return;
  String err;
  cfgSet(cfgAt(i), String(constrain(v, 0L, 65535L)), err);   // CT_INT 范围校验在 cfgSet 内
}

static void simTeStep(int ch, int d)   // 步进越界时循环（0↔65535）
{
  char k[8];
  snprintf(k, sizeof(k), "te.ch%d", ch);
  long v = simTeBase(ch) + d;
  if (v < 0) v = 65535;
  if (v > 65535) v = 0;
  simTeSet(k, v);
}

static void drawSimTeBody()
{
  bool run = sim::teOn();
  drawTextAt(14, centerBaseY(4, 28), "← 模拟遥测", COL_TEXT, COL_BG);
  String st = run ? "发帧中" : "已停止";
  drawTextAt(240, centerBaseY(4, 28), st, run ? COL_OK : COL_SUB, COL_BG);

  gfx->fillRoundRect(3, 36, 314, 44, 8, COL_PANEL);
  drawTextAt(14, centerBaseY(36, 44), String("通道数 ") + simTeCnt(), COL_TEXT, COL_PANEL);
  drawBtn(210, 42, 48, 32, "-", COL_KEY_FN, COL_TEXT);
  drawBtn(264, 42, 48, 32, "+", COL_KEY_FN, COL_TEXT);

  int n = simTeCnt();
  for (int i = 0; i < n; i++)
  {
    int y = 86 + i * 43;
    gfx->fillRoundRect(3, y, 314, 39, 8, COL_PANEL);
    char lb[16];
    snprintf(lb, sizeof(lb), "通道%d", i + 1);
    drawTextAt(14, centerBaseY(y, 39), lb, COL_SUB, COL_PANEL);
    char v[12];
    snprintf(v, sizeof(v), "%ld", simTeBase(i + 1));
    drawTextAt(96, centerBaseY(y, 39), v, COL_TEXT, COL_PANEL);
    drawBtn(208, y + 4, 48, 31, "-", COL_KEY_FN, COL_TEXT);
    drawBtn(262, y + 4, 48, 31, "+", COL_KEY_FN, COL_TEXT);
  }

  drawBtn(6, 436, 150, 40, run ? "发帧:开" : "发帧:关", run ? COL_OK : COL_KEY_FN, run ? 0xFFFF : COL_TEXT);
  drawBtn(162, 436, 152, 40, "返回", COL_KEY_FN, COL_TEXT);
}

static void handleSimTeTouch(bool press, bool release, bool down, int x, int y)
{
  (void)down;
  (void)release;
  if (press && ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_CONF); return; }

  if (press && ptIn(x, y, 210, 42, 48, 32)) { simTeSet("te.cnt", simTeCnt() - 1); dirtyFull = true; return; }
  if (press && ptIn(x, y, 264, 42, 48, 32)) { simTeSet("te.cnt", simTeCnt() + 1); dirtyFull = true; return; }

  if (press && y >= 86)
  {
    int i = (y - 86) / 43;
    if (i >= 0 && i < simTeCnt() && y < 86 + i * 43 + 39)
    {
      if (ptIn(x, y, 208, 86 + i * 43 + 4, 48, 31)) { simTeStep(i + 1, -10); dirtyFull = true; return; }
      if (ptIn(x, y, 262, 86 + i * 43 + 4, 48, 31)) { simTeStep(i + 1, 10); dirtyFull = true; return; }
    }
  }

  if (press && y >= 436)
  {
    String err;
    if (ptIn(x, y, 6, 436, 150, 40))
    {
      int i = cfgIndexOf("sim.te");
      if (i >= 0) cfgSet(cfgAt(i), sim::teOn() ? "0" : "1", err);   // cfgSet 内同步内存开关
      dirtyFull = true;
    }
    else if (ptIn(x, y, 162, 436, 152, 40))
      gotoScreen(S_CONF);
  }
}

static void drawDecodeBody()
{
  if (!re::hasRule())
  {
    gfx->fillRoundRect(3, 42, 314, 120, 8, COL_PANEL);
    drawTextAt(12, 70, "还没有协议规则", COL_TEXT, COL_PANEL);
    drawTextAt(12, 96, "手机 APP 连上后用 AI 分析报文", COL_SUB, COL_PANEL);
    drawTextAt(12, 120, "生成规则并下发（rule.set）即可在此显示", COL_SUB, COL_PANEL);
    drawBtn(3, 440, 314, 40, "返回", COL_KEY_FN, COL_TEXT);
    return;
  }

  // 规则信息卡
  gfx->fillRoundRect(3, 42, 314, 44, 8, COL_PANEL);
  drawTextAt(12, 54, "规则", COL_SUB, COL_PANEL);
  drawTextAt(52, 54, fitTail(re::ruleName(), 254), COL_ACCENT, COL_PANEL);
  String st = String("设备 ") + re::devCount() + " · 落盘 " + (lsActive() ? lsSession() : String("停"));
  drawTextAt(12, 76, fitTail(st, 296), COL_SUB, COL_PANEL);

  int top = 94;
  int nd = re::devCount();
  if (decDevSel >= nd) decDevSel = nd - 1;
  if (decDevSel < 0) decDevSel = 0;

  // 多设备：设备选择 chips
  if (nd > 1)
  {
    int w = min(58, 304 / nd);
    for (int i = 0; i < nd && i < 5; i++)
    {
      int x = 3 + i * (w + 4);
      String lab = "D" + String(re::devAddrAt(i));
      drawBtn(x, top, w, 30, lab.c_str(), i == decDevSel ? COL_ACCENT : COL_KEY_FN, i == decDevSel ? 0xFFFF : COL_TEXT);
    }
    top += 38;
  }

  // 当前值：两列（每行 26px），最多显示可见行数
  int rows = max(1, (300 - top) / 26);
  int nf = re::fieldCount();
  int dev = re::devAddrAt(decDevSel);
  uint32_t ms = re::curMs(dev);
  for (int r = 0; r < rows; r++)
  {
    for (int c = 0; c < 2; c++)
    {
      int fi = r * 2 + c;
      if (fi >= nf) break;
      int x = 3 + c * 158;
      gfx->fillRoundRect(x, top + r * 26, 154, 22, 5, COL_PANEL);
      drawTextAt(x + 8, top + r * 26 + 18, fitTail(re::fieldAt(fi).name, 70), COL_SUB, COL_PANEL);
      String v = re::curVal(dev, fi);
      if (v.length())
      {
        int vw = textWidth(v);
        drawTextAt(x + 148 - min(vw, 74), top + r * 26 + 18, v, COL_TEXT, COL_PANEL);
      }
      else drawTextAt(x + 148 - 36, top + r * 26 + 18, "--", COL_LINE, COL_PANEL);
    }
  }
  if (nd)
  {
    String t = "更新于 " + flTime(ms) + (nd > 1 ? String("  D") + dev : String(""));
    drawTextAt(12, 302, fitTail(t, 296), COL_SUB, COL_BG);
  }

  // 历史卡（下半）：显示 decLines 尾部若干行
  gfx->fillRoundRect(3, 314, 314, 118, 8, COL_PANEL);
  drawTextAt(12, 328, "历史", COL_SUB, COL_PANEL);
  int viewLines = 4;
  int total = (int)decLines.size();
  int maxScroll = max(0, total - viewLines) * 20;
  if (decScroll > maxScroll) decScroll = maxScroll;
  int startIdx = total - viewLines - decScroll / 20;
  if (startIdx < 0) startIdx = 0;
  for (int i = 0; i < viewLines && startIdx + i < total; i++)
    drawTextAt(12, 350 + i * 20, fitTail(decLines[startIdx + i], 296), COL_TEXT, COL_PANEL);
  if (total > viewLines)
    drawTextAt(266, 328, total - viewLines - decScroll / 20 > 0 ? "↑更多" : "到底", COL_SUB, COL_PANEL);

  drawBtn(3, 440, 100, 40, "返回", COL_KEY_FN, COL_TEXT);
  drawBtn(109, 440, 100, 40, re::pushOn() ? "推送:开" : "推送:关", re::pushOn() ? COL_OK : COL_KEY_FN, re::pushOn() ? 0xFFFF : COL_TEXT);
  drawBtn(215, 440, 102, 40, "清历史", COL_KEY_FN, COL_TEXT);
}

static void handleDecodeTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
    if (ptIn(x, y, 3, 440, 100, 40)) { gotoScreen(S_SETTINGS); return; }
    if (re::hasRule())
    {
      if (ptIn(x, y, 109, 440, 100, 40))
      {
        re::setPush(!re::pushOn());
        int i = cfgIndexOf("dec.push");
        if (i >= 0)
        {
          String e;
          cfgSet(cfgAt(i), re::pushOn() ? "1" : "0", e);
        }
        showToast(re::pushOn() ? "解码事件推送已开" : "解码事件推送已关");
        dirtyFull = true;
        return;
      }
      if (ptIn(x, y, 215, 440, 102, 40)) { decLines.clear(); decScroll = 0; dirtyFull = true; return; }

      // 设备 chips 点选
      int nd = re::devCount();
      if (nd > 1)
      {
        int w = min(58, 304 / nd);
        for (int i = 0; i < nd && i < 5; i++)
        {
          if (ptIn(x, y, 3 + i * (w + 4), 94, w, 30)) { decDevSel = i; dirtyFull = true; return; }
        }
      }
    }
  }
  // 历史区拖动上卷
  static int dragStartY = 0, scrollAtStart = 0;
  static bool dragging = false;
  static bool wasDown = false;
  if (down && y >= 314 && y < 432)
  {
    if (!wasDown) { dragging = true; dragStartY = y; scrollAtStart = decScroll; }
    if (dragging)
    {
      int dy = y - dragStartY;
      if (dy)
      {
        int total = (int)decLines.size();
        int maxScroll = max(0, total - 4) * 20;
        decScroll = scrollAtStart + dy;
        if (decScroll < 0) decScroll = 0;
        if (decScroll > maxScroll) decScroll = maxScroll;
        dirtyFull = true;
      }
    }
    wasDown = true;
    return;
  }
  else if (!down)
  {
    wasDown = false;
    dragging = false;
  }
}

static void drawRssi(int x, int y, int rssi)
{
  int lvl = rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : 1;
  for (int i = 0; i < 4; i++)
  {
    int bh = 4 + i * 3;
    uint16_t c = (i < lvl) ? COL_OK : COL_LINE;
    gfx->fillRect(x + i * 6, y + 12 - bh, 4, bh, c);
  }
}

static void drawWifiBody()
{
  drawBtn(3, 40, 110, 32, scanPending ? "扫描中.." : "重新扫描", COL_KEY_FN, COL_TEXT);
  String hint = scanPending ? "正在搜索附近网络..." : (apList.size() ? String("共 ") + apList.size() + " 个网络，点击选择" : String("未发现网络"));
  drawTextAt(122, centerBaseY(40, 32), hint.c_str(), COL_SUB, COL_BG);

  int contentH = (int)apList.size() * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (wifiScroll > maxScroll) wifiScroll = maxScroll;
  for (int i = 0; i < (int)apList.size(); i++)
  {
    int iy = LIST_TOP + i * ITEM_H - wifiScroll;
    if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
    gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
    drawTextAt(14, iy + 27, fitTail(apList[i].ssid, 190), COL_TEXT, COL_PANEL);
    String dbm = String(apList[i].rssi) + "dBm";
    drawTextAt(232, iy + 27, dbm, COL_SUB, COL_PANEL);
    drawRssi(292, iy + 10, apList[i].rssi);
  }
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, wifiScroll);
}

static void drawWifiPassBody()
{
  gfx->fillRoundRect(3, 42, 314, 52, 8, COL_PANEL);
  drawTextAt(14, 64, "网络:", COL_SUB, COL_PANEL);
  drawTextAt(14, 86, fitTail(selSsid, 180), COL_TEXT, COL_PANEL);
  drawBtn(227, 48, 82, 40, "连接", COL_ACCENT, 0xFFFF);
  if (wifiPassSaved)
    drawTextAt(14, 130, "已记住密码（明文显示，可直接点连接）", COL_OK, COL_BG);
  else
    drawTextAt(14, 130, "请在下方输入框输入密码", COL_SUB, COL_BG);
}

static void drawApiBody()
{
  if (editField >= 0)
  {
    static const char *titles[] = {"编辑接口地址", "编辑模型名称", "编辑 API Key"};
    drawTextAt(14, 58, titles[editField], COL_SUB, COL_BG);
    gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
    String v = (editField == 2) ? [] { String s; int n = utf8Count(editDraft); for (int i = 0; i < n; i++) s += '*'; return s; }() : editDraft;
    v += ((millis() / 500) & 1) ? "_" : "";
    drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
    drawBtn(3, 246, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
    drawBtn(217, 246, 100, 44, "保存", COL_ACCENT, 0xFFFF);
    return;
  }
  // 7 行（行高 46 步进 49）：厂商 / 接口地址 / API Key / 模型名称 / 拉取模型 / 保存 / 返回
  String keyMasked = cfg.key.length() ? cfg.key.substring(0, min(6, (int)cfg.key.length())) + "****" : "未设置";
  String vendorVal = cfgVendorName();
  {
    AiRec vr;
    if (aiRecGet(vendorVal, vr)) vendorVal += " 已存";
  }
  struct Card { int y; const char *label; String value; bool accent; };
  Card cards[7] = {
    {40, "厂商", vendorVal, false},
    {89, "接口地址", cfg.url, false},
    {138, "API Key", keyMasked, false},
    {187, "模型名称", cfg.model.length() ? cfg.model : String("未设置"), false},
    {236, "拉取模型列表", "自动获取该厂商模型", false},
    {285, "保存", cfgVendorName() == "自定义" ? String("保存设置") : String("保存并记住厂商"), true},
    {334, "返回主页", "", false},
  };
  for (auto &c : cards)
  {
    gfx->fillRoundRect(3, c.y, 314, 46, 8, c.accent ? COL_ACCENT : COL_PANEL);
    drawTextAt(14, c.y + 18, c.label, c.accent ? 0xFFFF : COL_SUB, c.accent ? COL_ACCENT : COL_PANEL);
    if (c.value.length())
      drawTextAt(90, c.y + 18, fitTail(c.value, 215), c.accent ? 0xFFFF : COL_TEXT, c.accent ? COL_ACCENT : COL_PANEL);
    if (strcmp(c.label, "返回主页") != 0)
      drawTextAt(296, c.y + 18, ">", c.accent ? 0xFFFF : COL_SUB, c.accent ? COL_ACCENT : COL_PANEL);
  }
}

static void drawChatIcon(int cx, int cy)
{
  gfx->fillRoundRect(cx - 20, cy - 16, 40, 28, 8, COL_ACCENT);
  gfx->fillTriangle(cx - 12, cy + 10, cx - 2, cy + 10, cx - 12, cy + 22, COL_ACCENT);
  gfx->fillCircle(cx - 9, cy - 2, 3, 0xFFFF);
  gfx->fillCircle(cx + 1, cy - 2, 3, 0xFFFF);
  gfx->fillCircle(cx + 11, cy - 2, 3, 0xFFFF);
}

static void drawMicIcon(int cx, int cy)
{
  uint16_t c = COL_ACCENT;
  gfx->fillRoundRect(cx - 8, cy - 20, 16, 26, 8, c);
  gfx->fillRect(cx - 13, cy - 6, 3, 10, c);
  gfx->fillRect(cx + 10, cy - 6, 3, 10, c);
  gfx->fillRect(cx - 13, cy + 2, 27, 3, c);
  gfx->fillRect(cx - 2, cy + 5, 4, 8, c);
  gfx->fillRect(cx - 8, cy + 13, 16, 3, c);
}

static void drawGearIcon(int cx, int cy)
{
  uint16_t c = COL_ACCENT;
  gfx->fillCircle(cx, cy, 13, c);
  for (int i = 0; i < 8; i++)
  {
    float a = i * 0.785398f;
    int x = cx + (int)(cosf(a) * 15.5f);
    int y = cy + (int)(sinf(a) * 15.5f);
    gfx->fillCircle(x, y, 4, c);
  }
  gfx->fillCircle(cx, cy, 6, COL_PANEL);
}

static void drawTermIcon(int cx, int cy)
{
  gfx->drawRoundRect(cx - 24, cy - 16, 48, 32, 6, COL_ACCENT);
  gfx->fillRect(cx - 22, cy - 7, 44, 2, COL_ACCENT);
  for (int i = 0; i < 2; i++) gfx->fillRect(cx - 18, cy + 2 + i * 8, 22 - i * 8, 2, COL_ACCENT);
  gfx->fillTriangle(cx + 2, cy - 4, cx + 18, cy + 4, cx + 2, cy + 12, COL_ACCENT);
}

static void drawBmsIcon(int cx, int cy)
{
  uint16_t c = COL_ACCENT;
  gfx->fillRoundRect(cx - 18, cy - 16, 36, 32, 6, c);
  gfx->fillRect(cx - 6, cy - 20, 12, 6, c);
  gfx->fillRect(cx - 14, cy + 10, 28, 4, c);
  gfx->fillRect(cx - 10, cy + 6, 4, 8, c);
  gfx->fillRect(cx + 6, cy + 6, 4, 8, c);
}

static void drawHomeBody()
{
  struct Tile { int x, y; const char *label; void (*icon)(int, int); };
  Tile tiles[5] = {
    {6, 96, "聊天", drawChatIcon},
    {106, 96, "串口", drawTermIcon},
    {206, 96, "小志", drawMicIcon},
    {6, 224, "电池", drawBmsIcon},
    {106, 224, "设置", drawGearIcon},
  };
  for (auto &t : tiles)
  {
    gfx->fillRoundRect(t.x, t.y, 94, 118, 14, COL_PANEL);
    gfx->drawRoundRect(t.x, t.y, 94, 118, 14, COL_LINE);
    t.icon(t.x + 47, t.y + 48);
    int tw = textWidth(t.label);
    drawTextAt(t.x + (94 - tw) / 2, t.y + 100, t.label, COL_TEXT, COL_PANEL);
  }
}

static void drawSerialBody()
{
  bool ble = (slMode() == LINK_BLE);
  bool on = slActive();
  drawBtn(3, 40, 100, 32, "BLE 模式", ble ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(110, 40, 100, 32, "TCP 模式", ble ? COL_KEY_FN : COL_ACCENT, 0xFFFF);
  drawBtn(217, 40, 100, 32, "清空记录", COL_KEY_FN, COL_TEXT);

  gfx->fillCircle(12, 82, 5, on ? COL_OK : 0xF980);
  drawTextAt(24, 87, fitTail(slStatus(), 280), on ? COL_TEXT : COL_SUB, COL_BG);
  String hint = ble ? "手机用 BLE 串口助手搜索并连接本设备"
                    : String("电脑或手机用 TCP 工具连 ") + slLocalIp() + ":" + String(SL_TCP_PORT);
  drawTextAt(10, 110, fitTail(hint, 300), COL_SUB, COL_BG);

  drawBtn(3, 122, 76, 32, flHexMode ? "HEX 开" : "HEX 关", flHexMode ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(83, 122, 76, 32, serialEol ? "换行 开" : "换行 关", serialEol ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(163, 122, 76, 32, "发送", COL_ACCENT, 0xFFFF);
  drawBtn(243, 122, 74, 32, "报文记录", COL_KEY_FN, COL_TEXT);

  int cb = chatViewBot();
  gfx->fillRoundRect(3, TERM_TOP, 314, cb - TERM_TOP, 8, COL_PANEL);
  buildTermLines();
  int viewH = cb - TERM_TOP - 6;
  int maxScroll = max(0, (int)termLines.size() * LINE_H - viewH);
  if (serialScroll > maxScroll) serialScroll = maxScroll;
  if (serialScroll < 0) serialScroll = 0;
  if (!termLines.size())
  {
    drawTextAt((SCREEN_W - textWidth("暂无数据")) / 2, TERM_TOP + 28, "暂无数据", COL_SUB, COL_PANEL);
    return;
  }
  int i0 = max(0, (serialScroll + LINE_H - 1) / LINE_H);
  for (int i = i0; i < (int)termLines.size(); i++)
  {
    int ly = TERM_TOP + 4 + i * LINE_H - serialScroll;
    if (ly + LINE_H > cb - 4) break;
    drawTextAt(10, ly + 17, termLines[i], termTx[i] ? COL_ACCENT : COL_TEXT, COL_PANEL);
  }
  drawScrollbar(SCREEN_W - 5, TERM_TOP + 4, viewH - 8, (int)termLines.size() * LINE_H, serialScroll);
}

static void drawFramesBody()
{
  String head = "共 " + String((int)frames.size()) + " 条记录，点击查看并 AI 分析";
  drawTextAt(12, 58, fitTail(head, 296), COL_SUB, COL_BG);
  int contentH = (int)frames.size() * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (framesScroll > maxScroll) framesScroll = maxScroll;
  for (int i = 0; i < (int)frames.size(); i++)
  {
    int iy = LIST_TOP + i * ITEM_H - framesScroll;
    if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
    FrameRec &f = frames[i];
    gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
    String l1 = String(f.rx ? "RX " : "TX ") + flTime(f.ms) + "  " + String(f.len) + " 字节";
    drawTextAt(12, iy + 15, l1, f.rx ? COL_TEXT : COL_ACCENT, COL_PANEL);
    String body = flHexMode ? f.hex : f.text;
    drawTextAt(12, iy + 32, fitTail(body.length() ? body : (flHexMode ? "(空)" : "(无可打印内容)"), 296), COL_SUB, COL_PANEL);
  }
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, framesScroll);
}

static void drawAnaBody()
{
  if (anaIdx < 0 || anaIdx >= (int)frames.size())
  {
    drawTextAt(12, 120, "报文已过期，请重新选择", COL_SUB, COL_BG);
    drawBtn(3, SCREEN_H - 48, 314, 40, "返回报文记录", COL_KEY_FN, COL_TEXT);
    return;
  }
  FrameRec f = frames[anaIdx];
  gfx->fillRoundRect(3, 42, 314, 76, 8, COL_PANEL);
  drawTextAt(12, 62, String(f.rx ? "接收 RX " : "发送 TX ") + flTime(f.ms) + "  " + String(f.len) + " 字节", COL_SUB, COL_PANEL);
  std::vector<String> hl;
  wrapLines(f.hex.length() ? f.hex : "(空)", 296, hl);
  for (int i = 0; i < (int)hl.size() && i < 3; i++)
    drawTextAt(12, 80 + i * 18, hl[i], COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, ANS_TOP, 314, ANS_BOT - ANS_TOP, 8, COL_PANEL);
  if (anaWorking)
  {
    drawTextAt(12, centerBaseY(ANS_TOP, 40), "AI 分析中，请稍候...", COL_ACCENT, COL_PANEL);
  }
  else if (anaErr.length())
  {
    drawTextAt(12, ANS_TOP + 20, fitTail("[错误] " + anaErr, 296), COL_ERR_TX, COL_PANEL);
  }
  else if (!anaLines.size())
  {
    drawTextAt(12, centerBaseY(ANS_TOP, 40), "点击下方按钮开始分析", COL_SUB, COL_PANEL);
  }
  else
  {
    int viewH = ANS_BOT - ANS_TOP - 8;
    int maxScroll = max(0, (int)anaLines.size() * LINE_H - viewH);
    if (anaScroll > maxScroll) anaScroll = maxScroll;
    if (anaScroll < 0) anaScroll = 0;
    int i0 = max(0, (anaScroll + LINE_H - 1) / LINE_H);
    for (int i = i0; i < (int)anaLines.size(); i++)
    {
      int ly = ANS_TOP + 4 + i * LINE_H - anaScroll;
      if (ly + LINE_H > ANS_BOT - 4) break;
      drawTextAt(10, ly + 17, anaLines[i], COL_TEXT, COL_PANEL);
    }
    drawScrollbar(SCREEN_W - 5, ANS_TOP + 4, viewH - 8, (int)anaLines.size() * LINE_H, anaScroll);
  }
  drawBtn(3, SCREEN_H - 48, 150, 40, "重新分析", COL_ACCENT, 0xFFFF);
  drawBtn(162, SCREEN_H - 48, 155, 40, "返回报文记录", COL_KEY_FN, COL_TEXT);
}

static void drawXiaozhiBody()
{
  gfx->fillRoundRect(3, 60, 314, 200, 12, COL_PANEL);
  drawMicIcon(160, 140);
  const char *l1 = "小志语音助手";
  const char *l2 = "集成开发中，敬请期待";
  drawTextAt((SCREEN_W - textWidth(l1)) / 2, 190, l1, COL_TEXT, COL_PANEL);
  drawTextAt((SCREEN_W - textWidth(l2)) / 2, 220, l2, COL_SUB, COL_PANEL);
}

static void drawBody()
{
  switch (scr)
  {
  case S_HOME:
    drawHomeBody();
    break;
  case S_XIAOZHI:
    drawXiaozhiBody();
    break;
  case S_SERIAL:
    drawSerialBody();
    break;
  case S_FRAMES:
    drawFramesBody();
    break;
  case S_AIANA:
    drawAnaBody();
    break;
  case S_OTA:
    drawOtaBody();
    break;
  case S_STATUS:
    drawStatusBody();
    break;
  case S_OTAURL:
    drawOtaUrlBody();
    break;
  case S_BMS:
    drawBmsBody();
    break;
  case S_CONF:
    drawConfBody();
    break;
  case S_BTMOD:
    drawBtmodBody();
    break;
  case S_RELAY:
    drawRelayBody();
    break;
  case S_MODELS:
    drawModelsBody();
    break;
  case S_DECODE:
    drawDecodeBody();
    break;
  case S_SIMIO:
    drawSimIoBody();
    break;
  case S_SIMTE:
    drawSimTeBody();
    break;
  case S_CHAT:
    chatRender(CHAT_TOP, chatViewBot() - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
    break;
  case S_SETTINGS:
    drawSettingsBody();
    break;
  case S_WIFI:
    drawWifiBody();
    break;
  case S_WIFIPASS:
    drawWifiPassBody();
    break;
  case S_API:
    drawApiBody();
    break;
  }
}

static void drawToast()
{
  int tw = textWidth(toastMsg);
  int w = min(300, tw + 28);
  int x = (SCREEN_W - w) / 2;
  gfx->fillRoundRect(x, 54, w, 36, 10, COL_PANEL_HI);
  gfx->drawRoundRect(x, 54, w, 36, 10, COL_ACCENT);
  drawTextAt(x + (w - tw) / 2, centerBaseY(54, 36), toastMsg.c_str(), 0xFFFF, COL_PANEL_HI);
}

static void drawClearDialog()
{
  gfx->fillRoundRect(30, 150, 260, 150, 12, COL_PANEL);
  gfx->drawRoundRect(30, 150, 260, 150, 12, COL_ACCENT);
  const char *msg = "确定清除所有聊天记录？";
  int tw = textWidth(msg);
  drawTextAt((SCREEN_W - tw) / 2, 205, msg, COL_TEXT, COL_PANEL);
  drawBtn(50, 240, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
  drawBtn(170, 240, 100, 44, "确定", COL_ERR_BG, COL_ERR_TX);
}

static void drawConnectingOverlay()
{
  gfx->fillRoundRect(30, 170, 260, 120, 12, COL_PANEL);
  gfx->drawRoundRect(30, 170, 260, 120, 12, COL_ACCENT);
  const char *l1 = "正在连接网络...";
  int tw = textWidth(l1);
  drawTextAt((SCREEN_W - tw) / 2, 220, l1, COL_TEXT, COL_PANEL);
  drawTextAt((SCREEN_W - textWidth(selSsid)) / 2, 250, selSsid, COL_SUB, COL_PANEL);
}

static bool kbVisible()
{
  return scr == S_CHAT || scr == S_WIFIPASS || (scr == S_API && editField >= 0) || scr == S_SERIAL || scr == S_OTAURL || (scr == S_CONF && confIdx >= 0) || (scr == S_BTMOD && btmodNameEdit);
}

static bool inputLineVisible()
{
  return scr == S_CHAT || scr == S_WIFIPASS || scr == S_SERIAL || scr == S_OTAURL;
}

static void compose()
{
  bool need = false;
  if (dirtyFull)
  {
    gfx->fillScreen(COL_BG);
    drawTitleBar();
    drawBody();
    if (inputLineVisible()) kbDrawInputLine();
    if (kbVisible()) kbDraw();
    dirtyChat = dirtyInput = false;
    need = true;
  }
  else
  {
    if (dirtyChat && scr == S_CHAT)
    {
      chatRender(CHAT_TOP, chatViewBot() - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
      need = true;
    }
    if (dirtyInput && inputLineVisible())
    {
      kbDrawInputLine();
      need = true;
    }
  }
  if (wcConnecting && scr == S_WIFIPASS) { drawConnectingOverlay(); need = true; }
  if (dlgClear) { drawClearDialog(); need = true; }
  if (pickOpen) { drawPickDialog(); need = true; }
  if (millis() < toastUntil) { drawToast(); need = true; }
  else if (needRestore) { needRestore = false; dirtyFull = true; }
  if (need) gfx->flush();
}

static void enterEdit(int field)
{
  editField = field;
  kbSetMask(field == 2);
  kbSetSendLabel("完成");
  kbAttach(&editDraft);
  kbReset();
  editDraft = (field == 0) ? cfg.url : (field == 1) ? cfg.model : cfg.key;
  dirtyFull = true;
}

static void exitEdit(bool save)
{
  if (save)
  {
    editDraft.trim();
    if (editField == 0) cfg.url = apiNormalizeBase(editDraft);   // 误填完整端点也存成基础地址
    else if (editField == 1) cfg.model = editDraft;
    else cfg.key = editDraft;
    cfgSaveApi();
    if (editField == 0) logTeePrintf("[cfg] url saved=%s\n", cfg.url.c_str());
    showToast("已保存");
  }
  editField = -1;
  kbAttach(&draft);
  kbSetMask(false);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

static void handleChatTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 254, 4, 62, 28) || ptIn(x, y, 222, 8, 28, 20))
    {
      gotoScreen(S_HOME);
      return;
    }
  }
  int cb = chatViewBot();
  if (down && !press && y >= CHAT_TOP && y < cb)
  {
    if (!chatDragging)
    {
      chatDragging = true;
      dragStartY = y;
      scrollAtDragStart = chatScroll;
    }
    int dy = y - dragStartY;
    if (dy)
    {
      int contentH = chatContentHeight(aiBusy());
      int maxScroll = max(0, contentH - (cb - CHAT_TOP));
      // 内容要跟手：手指下滑（dy>0）scroll 增大，渲染时内容下移露出更早消息（chatScroll=距底部距离，与列表页的距顶部语义相反）
      chatScroll = scrollAtDragStart + dy;
      if (chatScroll < 0) chatScroll = 0;
      if (chatScroll > maxScroll) chatScroll = maxScroll;
      dirtyChat = true;
    }
  }
  if (!down) chatDragging = false;
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND) sendToAi();
}

static void handleSettingsTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 3, 4, 56, 28)) { releaseGuard = true; gotoScreen(S_HOME); return; }
  if (ptIn(x, y, 3, 40, 314, 36)) { releaseGuard = true; scanStart(); gotoScreen(S_WIFI); return; }
  if (ptIn(x, y, 3, 76, 314, 36)) { releaseGuard = true; gotoScreen(S_API); return; }
  if (ptIn(x, y, 3, 112, 314, 36)) { releaseGuard = true; gotoScreen(S_BTMOD); return; }
  if (ptIn(x, y, 3, 148, 314, 36)) { releaseGuard = true; gotoScreen(S_RELAY); return; }
  if (ptIn(x, y, 3, 184, 314, 36)) { releaseGuard = true; gotoScreen(S_DECODE); return; }
  if (ptIn(x, y, 3, 220, 314, 36)) { releaseGuard = true; gotoScreen(S_BMS); return; }
  if (ptIn(x, y, 3, 256, 314, 36)) { releaseGuard = true; gotoScreen(S_OTA); return; }
  if (ptIn(x, y, 3, 292, 314, 36)) { releaseGuard = true; gotoScreen(S_STATUS); return; }
  if (ptIn(x, y, 3, 328, 314, 36)) { releaseGuard = true; confGroup = -1; confScroll = 0; gotoScreen(S_CONF); return; }
  if (ptIn(x, y, 3, 364, 314, 36)) { dlgClear = true; dirtyFull = true; return; }
  if (ptIn(x, y, 3, 400, 314, 36))
  {
    showToast("正在重启...");
    gfx->flush();
    delay(500);
    ESP.restart();
  }
  if (ptIn(x, y, 3, 436, 314, 36)) { gotoScreen(S_HOME); }
}

static void handleWifiTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
    if (ptIn(x, y, 3, 40, 110, 32)) { scanStart(); return; }
  }
  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = wifiScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int contentH = (int)apList.size() * ITEM_H;
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, contentH - viewH);
      wifiScroll = scrollAtDragStart - dy;
      if (wifiScroll < 0) wifiScroll = 0;
      if (wifiScroll > maxScroll) wifiScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    int idx = (y + wifiScroll - LIST_TOP) / ITEM_H;
    if (idx >= 0 && idx < (int)apList.size())
    {
      selSsid = apList[idx].ssid;
      kbAttach(&passDraft);
      kbReset();
      String saved;
      wifiPassSaved = wifiRecGet(selSsid, saved);
      kbSetMask(!wifiPassSaved);   // 已记住：明文显示（可查看密码）；否则打码输入
      kbSetSendLabel("完成");
      gotoScreen(S_WIFIPASS);
      if (wifiPassSaved) passDraft = saved;   // kbReset 之后回填记住的密码
    }
  }
}

static void handleWifiPassTouch(bool press, bool down, int x, int y)
{
  if (wcConnecting) return;
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28))
    {
      releaseGuard = true;
      kbReset();
      kbAttach(&draft);
      kbSetMask(false);
      kbSetSendLabel("发送");
      gotoScreen(S_WIFI);
      return;
    }
    if (ptIn(x, y, 227, 48, 82, 40))
    {
      String pw = passDraft;
      pw.trim();
      if (!pw.length()) { showToast("请输入密码"); return; }
      passDraft.clear();
      dirtyInput = true;
      startWifiConnect(selSsid, pw);
      dirtyFull = true;
      return;
    }
  }
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND)
  {
    String pw = passDraft;
    pw.trim();
    if (pw.length())
    {
      passDraft.clear();
      dirtyInput = true;
      startWifiConnect(selSsid, pw);
      dirtyFull = true;
    }
  }
}

// 拉取当前厂商的模型列表；manualFallback=失败时进入键盘手动输入模型
static void apiFetchModels(bool manualFallback)
{
  if (!wifiUp()) { showToast("请先连接 WiFi", 2200); return; }
  if (!cfg.key.length()) { showToast("请先填 API Key", 2200); return; }
  showToast("拉取模型中...", 3000);
  dirtyFull = true;
  String ms, err;
  if (aiFetchModels(cfg.url, cfg.key, ms, err))
  {
    cfgSetModels(ms);
    String vn = cfgVendorName();
    if (vn != "自定义")
    {
      AiRec r{vn, cfg.url, cfg.key, cfg.model, ms};
      aiRecUpsert(r);
    }
    modelsScroll = 0;
    gotoScreen(S_MODELS);
  }
  else
  {
    showToast("拉取失败: " + err, 2800);
    if (manualFallback) enterEdit(1);   // 兜底：键盘手动填模型名
  }
}

// S_MODELS：模型列表（来自拉取或切厂商记录恢复），点选即用
static void drawModelsBody()
{
  int cnt = modelsCount();
  if (!cnt)
  {
    drawTextAt(14, 130, "暂无模型列表，回到上一页点「拉取模型列表」", COL_SUB, COL_BG);
    return;
  }
  int contentH = cnt * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (modelsScroll > maxScroll) modelsScroll = maxScroll;
  if (modelsScroll < 0) modelsScroll = 0;
  for (int i = 0; i < cnt; i++)
  {
    int iy = LIST_TOP + i * ITEM_H - modelsScroll;
    if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
    String id = modelsAt(i);
    bool cur = (id == cfg.model);
    gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, cur ? COL_ACCENT : COL_PANEL);
    drawTextAt(14, iy + 24, fitTail(id, 290), cur ? 0xFFFF : COL_TEXT, cur ? COL_ACCENT : COL_PANEL);
    if (cur) drawTextAt(270, iy + 24, "使用中", 0xFFFF, COL_ACCENT);
  }
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, modelsScroll);
}

static void handleModelsTouch(bool press, bool release, bool down, int x, int y)
{
  if (press && ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_API); return; }

  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = modelsScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, modelsCount() * ITEM_H - viewH);
      modelsScroll = scrollAtDragStart - dy;
      if (modelsScroll < 0) modelsScroll = 0;
      if (modelsScroll > maxScroll) modelsScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;

  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    int idx = (y + modelsScroll - LIST_TOP) / ITEM_H;
    if (idx >= 0 && idx < modelsCount())
    {
      cfg.model = modelsAt(idx);
      cfgSaveApi();
      String vn = cfgVendorName();
      if (vn != "自定义")
      {
        AiRec r{vn, cfg.url, cfg.key, cfg.model, cfgModels()};
        aiRecUpsert(r);
      }
      showToast("已选 " + cfg.model);
      gotoScreen(S_API);
    }
  }
}

static void handleApiTouch(bool press, bool down, int x, int y)
{
  if (pickOpen) { handlePickTouch(press, x, y); return; }
  if (editField >= 0)
  {
    if (press)
    {
      if (ptIn(x, y, 3, 4, 56, 28)) { exitEdit(false); return; }
      if (ptIn(x, y, 3, 246, 100, 44)) { exitEdit(false); return; }
      if (ptIn(x, y, 217, 246, 100, 44)) { exitEdit(true); return; }
    }
    uint8_t ev = kbHandle(down, x, y);
    if (ev & KB_CHANGED) dirtyFull = true;
    if (ev & KB_SEND) exitEdit(true);
    return;
  }
  if (!press) return;
  if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
  if (ptIn(x, y, 3, 40, 314, 46))   // 厂商：弹窗选择（原循环切换废弃）
  {
    int vi = cfgIndexOf("ai.vendor");
    if (vi >= 0)
    {
      const CfgItem &it = cfgAt(vi);
      pickOpenDlg(1, "选择 AI 厂商", it.options, cfgGetRaw(it), it);
    }
    return;
  }
  if (ptIn(x, y, 3, 89, 314, 46)) { enterEdit(0); return; }   // 接口地址
  if (ptIn(x, y, 3, 138, 314, 46)) { enterEdit(2); return; }  // API Key
  if (ptIn(x, y, 3, 187, 314, 46)) { apiFetchModels(true); return; }  // 模型：有列表选列表，没有就拉
  if (ptIn(x, y, 3, 236, 314, 46)) { apiFetchModels(false); return; } // 拉取模型列表
  if (ptIn(x, y, 3, 285, 314, 46))
  {
    cfgSaveApi();
    String vn = cfgVendorName();
    if (vn != "自定义")
    {
      AiRec r{vn, cfg.url, cfg.key, cfg.model, cfgModels()};
      aiRecUpsert(r);
      showToast("已保存，下次切回直接用");
    }
    else showToast("已保存");
    dirtyFull = true;
    return;
  }
  if (ptIn(x, y, 3, 334, 314, 46)) { gotoScreen(S_SETTINGS); }
}

static void handleDialogTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 50, 240, 100, 44)) { dlgClear = false; dirtyFull = true; return; }
  if (ptIn(x, y, 170, 240, 100, 44))
  {
    dlgClear = false;
    chatClear();
    chatScroll = 0;
    showToast("聊天记录已清除");
    dirtyFull = true;
  }
}

static void handleSerialTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_HOME); return; }
    if (ptIn(x, y, 3, 40, 100, 32)) { slSetMode(LINK_BLE); serialScroll = 0; dirtyFull = true; return; }
    if (ptIn(x, y, 110, 40, 100, 32)) { slSetMode(LINK_TCP); serialScroll = 0; dirtyFull = true; return; }
    if (ptIn(x, y, 217, 40, 100, 32)) { flClear(); serialScroll = 0; dirtyFull = true; showToast("记录已清空"); return; }
    if (ptIn(x, y, 3, 122, 76, 32)) { flToggleHex(); dirtyFull = true; return; }
    if (ptIn(x, y, 83, 122, 76, 32)) { serialEol = !serialEol; dirtyFull = true; return; }
    if (ptIn(x, y, 163, 122, 76, 32)) { serialSend(); return; }
    if (ptIn(x, y, 243, 122, 74, 32)) { framesScroll = 0; gotoScreen(S_FRAMES); return; }
  }
  int cb = chatViewBot();
  if (down && !press && y >= TERM_TOP && y < cb)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = serialScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      buildTermLines();
      int viewH = cb - TERM_TOP - 6;
      int maxScroll = max(0, (int)termLines.size() * LINE_H - viewH);
      // 串口屏与其他列表页同为"距顶部"语义：手指下滑（dy>0）scroll 减小，内容跟手下移
      serialScroll = scrollAtDragStart - dy;
      if (serialScroll < 0) serialScroll = 0;
      if (serialScroll > maxScroll) serialScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND) serialSend();
}

static void handleFramesTouch(bool press, bool release, bool down, int x, int y)
{
  if (press && ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SERIAL); return; }
  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = framesScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, (int)frames.size() * ITEM_H - viewH);
      framesScroll = scrollAtDragStart - dy;
      if (framesScroll < 0) framesScroll = 0;
      if (framesScroll > maxScroll) framesScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    int idx = (y + framesScroll - LIST_TOP) / ITEM_H;
    if (idx >= 0 && idx < (int)frames.size()) startAnalyze(idx);
  }
}

static void handleAnaTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_FRAMES); return; }
    if (ptIn(x, y, 3, SCREEN_H - 48, 150, 40)) { startAnalyze(anaIdx); return; }
    if (ptIn(x, y, 162, SCREEN_H - 48, 155, 40)) { gotoScreen(S_FRAMES); return; }
  }
  if (down && !press && y >= ANS_TOP && y < ANS_BOT)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = anaScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = ANS_BOT - ANS_TOP - 8;
      int maxScroll = max(0, (int)anaLines.size() * LINE_H - viewH);
      anaScroll = scrollAtDragStart - dy;
      if (anaScroll < 0) anaScroll = 0;
      if (anaScroll > maxScroll) anaScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
}

static void handleOtaTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
    if (ptIn(x, y, 3, 338, 154, 44))
    {
      otaSetArmed(!otaArmed());
      showToast(otaArmed() ? "已开启推送接收" : "已关闭推送接收");
      dirtyFull = true;
      return;
    }
    if (ptIn(x, y, 163, 338, 154, 44))
    {
      kbAttach(&otaUrlEdit);
      kbSetMask(false);
      kbSetSendLabel("保存");
      kbReset();
      otaUrlEdit = otaUrl;    // kbReset 会清空绑定缓冲，之后重新赋值
      gotoScreen(S_OTAURL);
      return;
    }
    if (ptIn(x, y, 3, 390, 314, 44))
    {
      if (otaBusy()) { showToast("升级进行中"); return; }
      if (!wifiUp()) { showToast("未联网，请先配置 WiFi"); return; }
      if (otaWebStart(otaUrl))
      {
        showToast("已开始升级", 1200);
        dirtyFull = true;
      }
      else
      {
        showToast(otaMessage().length() ? otaMessage() : "无法开始升级", 2600);
        dirtyFull = true;
      }
      return;
    }
  }
  if (otaBusy() && otaState() == OST_RUNNING)
  {
    uint8_t ph = (millis() / 450) % 3;
    if (ph != lastOtaPhase) { lastOtaPhase = ph; dirtyFull = true; }
  }
}

static void leaveOtaUrl()
{
  // 先重置键盘（清空当前绑定的编辑缓冲），再绑回聊天草稿，避免清掉用户输入
  kbReset();
  kbAttach(&draft);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

static void otaUrlCommit(bool alsoStart)
{
  otaUrlEdit.trim();
  otaUrl = otaUrlEdit;
  cfg.otaUrl = otaUrl;
  cfgSaveOtaUrl();
  leaveOtaUrl();
  gotoScreen(S_OTA);
  if (!alsoStart)
  {
    showToast("地址已保存");
    return;
  }
  if (!wifiUp())
  {
    showToast("未联网，请先配置 WiFi", 2600);
    return;
  }
  if (otaWebStart(otaUrl)) showToast("已开始 URL 升级", 1500);
  else showToast(otaMessage().length() ? otaMessage() : "无法开始升级", 2600);
  dirtyFull = true;
}

static void handleOtaUrlTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28) || ptIn(x, y, 3, 246, 100, 44))
    {
      leaveOtaUrl();      // 返回：丢弃这次编辑
      gotoScreen(S_OTA);
      return;
    }
    if (ptIn(x, y, 110, 246, 100, 44))
    {
      otaUrlCommit(false);
      return;
    }
    if (ptIn(x, y, 217, 246, 100, 44))
    {
      otaUrlCommit(true);
      return;
    }
  }
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyFull = true;
  if (ev & KB_SEND) otaUrlCommit(false);
}

static void handleStatusTouch(bool press, int x, int y)
{
  if (press && ptIn(x, y, 3, 4, 56, 28)) gotoScreen(S_SETTINGS);
}

static void handleHomeTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 6, 96, 94, 118)) { gotoScreen(S_CHAT); return; }
  if (ptIn(x, y, 106, 96, 94, 118)) { serialScroll = 0; gotoScreen(S_SERIAL); return; }
  if (ptIn(x, y, 206, 96, 94, 118)) { gotoScreen(S_XIAOZHI); return; }
  if (ptIn(x, y, 6, 224, 94, 118)) { gotoScreen(S_BMS); return; }
  if (ptIn(x, y, 106, 224, 94, 118)) { gotoScreen(S_SETTINGS); return; }
}

static void routeTouch(bool press, bool release, bool down, int x, int y)
{
  if (release && releaseGuard) { releaseGuard = false; return; } // 跳页/退出编辑当帧吞掉抬起
  if (dlgClear) { handleDialogTouch(press, x, y); return; }
  switch (scr)
  {
  case S_HOME: handleHomeTouch(press, x, y); break;
  case S_XIAOZHI:
    if (press && ptIn(x, y, 3, 4, 56, 28)) gotoScreen(S_HOME);
    break;
  case S_CHAT: handleChatTouch(press, release, down, x, y); break;
  case S_SERIAL: handleSerialTouch(press, release, down, x, y); break;
  case S_FRAMES: handleFramesTouch(press, release, down, x, y); break;
  case S_AIANA: handleAnaTouch(press, down, x, y); break;
  case S_OTA: handleOtaTouch(press, down, x, y); break;
  case S_OTAURL: handleOtaUrlTouch(press, down, x, y); break;
  case S_STATUS: handleStatusTouch(press, x, y); break;
  case S_CONF: handleConfTouch(press, release, down, x, y); break;
  case S_BTMOD: handleBtmodTouch(press, release, down, x, y); break;
  case S_MODELS: handleModelsTouch(press, release, down, x, y); break;
  case S_DECODE: handleDecodeTouch(press, release, down, x, y); break;
  case S_SIMIO: handleSimIoTouch(press, release, down, x, y); break;
  case S_SIMTE: handleSimTeTouch(press, release, down, x, y); break;
  case S_RELAY:
    if (press)
    {
      if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_HOME); return; }
      if (ptIn(x, y, 3, 440, 314, 40)) { gotoScreen(S_HOME); return; }
      if (ptIn(x, y, 3, 104, 154, 40))   // 中继开关
      {
        int i = cfgIndexOf("relay.on");
        if (i >= 0)
        {
          String e;
          bool turnOn = !relayEnabled();
          cfgSet(cfgAt(i), turnOn ? "1" : "0", e);
          if (turnOn) relayRescan();   // 开关刚打开就扫一轮，不用等 8 秒重扫定时
        }
        dirtyFull = true;
        return;
      }
      if (ptIn(x, y, 163, 104, 154, 40)) { relayRescan(); dirtyFull = true; return; }
      if (relayConnected())
      {
        if (ptIn(x, y, 3, 268, 102, 40)) { gotoScreen(S_SERIAL); return; }
        if (ptIn(x, y, 109, 268, 102, 40)) { gotoScreen(S_DECODE); return; }
        if (ptIn(x, y, 215, 268, 102, 40)) { relayDisconnect(); dirtyFull = true; return; }
      }
      else   // 点列表行 → 连接该设备
      {
        int rowY = 186;
        int n = relayScanCount();
        for (int i = 0; i < n && i < 4; i++)
        {
          if (ptIn(x, y, 3, rowY, 314, 38))
          {
            ScanItem it = relayScanAt(i);
            relayConnectAddr(it.addr);
            dirtyFull = true;
            return;
          }
          rowY += 44;
        }
      }
    }
    break;
  case S_BMS:
    if (press) {
      if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_HOME); return; }
      if (ptIn(x, y, 163, 440, 154, 40)) { gotoScreen(S_HOME); return; }   // 返回（右半）
      if (ptIn(x, y, 3, 440, 154, 40)) { bms::rescan(); dirtyFull = true; return; }   // 重新扫描（左半）
      if (!bms::hasData())   // fix14：点列表行 → 连接该设备
      {
        int rowY = 140;
        int n = bms::scanCount();
        for (int i = 0; i < n && i < 5; i++)
        {
          if (ptIn(x, y, 3, rowY, 314, 38))
          {
            bms::ScanItem it = bms::scanAt(i);
            bms::connectAddr(it.addr);
            dirtyFull = true;
            return;
          }
          rowY += 44;
        }
      }
    }
    break;
  case S_SETTINGS: handleSettingsTouch(press, x, y); break;
  case S_WIFI: handleWifiTouch(press, release, down, x, y); break;
  case S_WIFIPASS: handleWifiPassTouch(press, down, x, y); break;
  case S_API: handleApiTouch(press, down, x, y); break;
  }
}

#define STK(tag) BOOTLOG("[stk] " tag " stack=%u heap=%u largest=%u\n", \
                         (unsigned)uxTaskGetStackHighWaterMark(NULL),   \
                         (unsigned)ESP.getFreeHeap(),                   \
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))

void setup()
{
  Serial0.begin(115200);
  Serial.begin(115200);
  delay(300);
  logTeeBegin();
  { // 复位原因人话：一眼分诊——3=PANIC(内存踩踏) 4/5=看门狗(任务) 6=硬件看门狗(系统挂死) 7=掉电
    static const char *rstName[9] = {"未知", "上电", "软件重启", "PANIC崩溃", "中断看门狗",
                                     "任务看门狗", "硬件看门狗(系统挂死)", "掉电", "SDIO"};
    int r = (int)esp_reset_reason();
    BOOTLOG("\n[boot] setup enter rst=%d (%s)\n", r, (r >= 0 && r < 9) ? rstName[r] : "未知");
  }
  STK("enter");
  { // 飞行记录仪：上次 AI 请求中途崩溃的话，NVS 里有崩溃前日志，开机读回打印
    // 同时把上次崩溃挪到 crash2 槽位：连崩两次（中间那次开机会清 crash）也不丢第一次现场
    bool v = false, v2 = false;
    String prev2 = logTeePersistLoadKey("crash2", &v2);
    String s = logTeePersistLoad(&v);
    if (v && s.length())
    {
      logTeePrintf("[crash] 上次请求未正常结束（设备已重启），崩溃前日志：\n");
      logTeePrint(s);
      logTeePrintf("[crash] ——以上为崩溃前日志——\n");
      logTeePersistSaveKey("crash2", s);
    }
    if (v2 && prev2.length())
    {
      logTeePrintf("[crash] ——再上一次崩溃的日志——\n");
      logTeePrint(prev2);
      logTeePrintf("[crash] ——以上为再上一次崩溃——\n");
    }
    logTeePersistClear();
  }
  cfgLoad();
  cfgInit();    // 配置表：生成网页 PIN、载入新增设置项
  otaUrl = cfg.otaUrl;
  BOOTLOG("[boot] stage: cfg ok\n");

  ui_init();
  BOOTLOG("[boot] stage: ui ok\n");
  drawSplash("正在启动...");
  otaSdUpdate();
  touchInit();
  aiBegin();
  flBegin();
  flSetTap([](bool rx, uint32_t ms, const uint8_t *d, int n) {
    lsFrame(rx, ms, d, n);   // frames.jsonl（会话开着才写）
    if (!rx) return;
    re::DecResult r;
    re::tapFrame(rx, ms, d, n, r);   // 规则解码 + evt:dec 推送（内部判 dec.push）
    if (!r.ok) return;
    lsDecoded(ms, r.dev, r.csvVals);
    String h = flTime(ms);
    if (re::devCount() > 1) h += " D" + String(r.dev);
    decPushHistory(h + " " + r.line);
    if (scr == S_DECODE) dirtyFull = true;
  });
  BOOTLOG("[boot] stage: ai ok\n");
  STK("ai");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  drawSplash("初始化电池监测...");
  bms::begin();
  STK("wifi-mode");
  drawSplash("启动串口服务...");
  slBegin("ESP32-S3-AI");     // 建 server + NUS 服务（不广播）
  drawSplash("初始化蓝牙中继...");
  relayBegin();               // 蓝牙主机客户端（连蓝牙串口模块收报文）
  acInit();                   // NUS 控制行协议（接管 ctrl 钩子，透传仍走 rxHook）
  lsBegin();                  // TF 落盘（挂载在开会话时才做）
  re::reBegin();              // 从 TF 恢复上次激活的解码规则
  sim::begin();               // 模拟开关量帧（cfg sim.io）
  STK("ble");
  drawSplash("启动 OTA...");
  otaBleBegin();              // 建蓝牙 OTA 服务（不广播）
  BOOTLOG("[boot] stage: ble svc ok=%d\n", (int)otaBleReady());
  otaBegin();                 // ArduinoOTA（网络推送）
  otaHttpBegin();             // WiFi 推送升级（网页 POST 固件到 8080）
  slStartAdv();               // 所有 GATT 服务就位后再开广播
  BOOTLOG("[boot] stage: ble adv ok\n");
  STK("ota");
  kbAttach(&draft);
  kbSetSendLabel("发送");

  drawSplash("正在启动...");

  bool ok = false;
  if (cfg.ssid.length())
  {
    drawSplash("正在连接 " + cfg.ssid);
    WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
    uint32_t t0 = millis();
    while (millis() - t0 < 5000)
    {
      if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
      delay(150);
    }
  }
  scr = S_HOME;
  if (ok)
  {
    drawSplash("IP: " + WiFi.localIP().toString());
    delay(1200);
    showToast("WiFi 已连接");
  }
  else
  {
    showToast(cfg.ssid.length() ? "WiFi 连接失败，请在设置中检查" : "未联网，请在设置中配置 WiFi", 3000);
  }
  dirtyFull = true;
}

void loop()
{
  static bool stLogged = false;
  if (!stLogged) { stLogged = true; STK("loop-first"); }

  btmodDebugPump();   // AT 口调试模式开启时，及时收取 41/42 上的字节

  TouchPoint tp;
  bool down = touchRead(tp) && tp.pressed;
  int x, y;
  if (down) { x = tp.x; y = tp.y; }
  else { x = lastX; y = lastY; }

  bool press = down && !lastDown;
  bool release = !down && lastDown;
  if (press) { downX = x; downY = y; movedDist = 0; }
  if (down)
  {
    int dx = abs(x - downX) + abs(y - downY);
    if (dx > movedDist) movedDist = dx;
  }

  routeTouch(press, release, down, x, y);
  lastDown = down;
  lastX = x;
  lastY = y;

  {   // 接口地址哨兵：任何代码改动 cfg.url 都在这里现形（曾复现运行期变成完整端点导致拉模型 404）
    static String urlSentinel;
    if (cfg.url != urlSentinel)
    {
      bool first = urlSentinel.length() == 0 && cfg.url.length() > 0;
      urlSentinel = cfg.url;
      if (!first) logTeePrintf("[cfg] URL CHANGED -> %s\n", cfg.url.c_str());
    }
  }

  serviceAi();
  serviceScan();
  serviceWifiConnect();
  slPoll();
  otaPoll();
  otaHttpPoll();   // 把 WiFi 升级地址同步到蓝牙"信息"特征，网页据此自动填 IP
  bms::poll();
  relayPoll();
  sim::poll();   // 模拟开关量帧（开启时每 1.5s 一帧：上屏/落盘/解码/发手机）

  // TF 会话：中继连上自动开，断开收尾（APP 也可 log.start/stop 手动控制）
  lsPoll();
  {
    static bool lastRelayConn = false;
    bool rc = relayConnected();
    if (rc && !lastRelayConn && !lsActive()) lsStart("relay");
    else if (!rc && lastRelayConn && lsActive()) lsStop();
    lastRelayConn = rc;
  }

  // BMS 扫描页：列表实时在变，未连接时每 500ms 重绘一次
  if (scr == S_BMS && !bms::hasData())
  {
    static uint32_t lastBmsMs = 0;
    if (millis() - lastBmsMs > 500) { lastBmsMs = millis(); dirtyFull = true; }
  }

  // 蓝牙中继页：扫描/连接状态实时在变，未连接时每 500ms 重绘一次
  if (scr == S_RELAY && !relayConnected())
  {
    static uint32_t lastRelayMs = 0;
    if (millis() - lastRelayMs > 500) { lastRelayMs = millis(); dirtyFull = true; }
  }

  uint8_t os = (uint8_t)otaState();
  int op = otaPercent();
  if (os != lastOtaState || op != lastOtaPct)
  {
    lastOtaState = os;
    lastOtaPct = op;
    if (scr == S_OTA) dirtyFull = true;
  }

  // 设备状态页：每 500ms 刷新一次（蓝牙连接/广播、升级进度都是实时变的）
  if (scr == S_STATUS)
  {
    static uint32_t lastStatMs = 0;
    if (millis() - lastStatMs > 500)
    {
      lastStatMs = millis();
      dirtyFull = true;
    }
  }

  if (frames.size() != lastFrameCount)
  {
    lastFrameCount = frames.size();
    if (scr == S_SERIAL || scr == S_FRAMES) dirtyFull = true;
  }
  if (scr == S_SERIAL)
  {
    String st = slStatus();
    if (st != lastSlStatus)
    {
      lastSlStatus = st;
      dirtyFull = true;
    }
  }

  if (aiBusy() && scr == S_CHAT)
  {
    uint8_t phase = (millis() / 450) % 3;
    if (phase != lastBusyPhase) { lastBusyPhase = phase; dirtyChat = true; }
  }
  else lastBusyPhase = 0;

  uint8_t blink = (millis() / 500) & 1;
  if (blink != lastBlink)
  {
    lastBlink = blink;
    if (inputLineVisible() || editField >= 0) dirtyInput = true;
    if (editField >= 0) dirtyFull = true;
  }

  if (millis() - lastWatchdog > 15000)
  {
    lastWatchdog = millis();
    // 在 WiFi 选择页时不自动 begin：否则重连会再次占住射频，导致“重新扫描”失败
    if (cfg.ssid.length() && !wifiUp() && !wcConnecting && !scanPending
        && scr != S_WIFI && scr != S_WIFIPASS)
      WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
  }

  compose();
}
