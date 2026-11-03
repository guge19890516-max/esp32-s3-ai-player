#include "serial_link.h"
#include "frame_log.h"
#include "log_tee.h"
#include "ota_update.h"   // slBleResume 重建后需重注册蓝牙 OTA 服务
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#if __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif
#include <NimBLEDevice.h>
// resume 复用路径需要：补做 createServer 首次新建时才做的 GATT 库表初始化
#include "nimble/nimble/host/include/host/ble_gatt.h"
#include "nimble/nimble/host/include/host/ble_att.h"
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include <esp_heap_caps.h>

static const char *SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char *TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static const int RX_MAX = 256;     // 一次 BLE 写的最大字节数（MTU 247 → 244 字节，不能截断）
static const int TX_CHUNK = 128;   // BLE 通知分片
static const int TX_QUEUE_N = 16;  // 待发分片队列深度
static const int TX_GAP_MS = 10;   // 分片间隔，避免打爆 BLE 发送缓冲

struct RxItem
{
  uint16_t len;
  uint8_t data[RX_MAX];
};

struct TxItem
{
  uint16_t len;
  uint8_t data[TX_CHUNK];
};

static QueueHandle_t rxQ;
static NimBLECharacteristic *txChar = nullptr;
static NimBLEServer *bleSrv = nullptr;
static bool bleConnected = false;
static WiFiServer *tcpSrv = nullptr;
static WiFiClient tcpCli;
static bool tcpListening = false;
static LinkMode mode = LINK_BLE;
static char devName[24] = "ESP32-AI";
static bool bleReady = false;
static volatile bool bleDown = false;   // BLE 栈挂起中（TLS 握手期间），回调须跳过 BLE 操作
static volatile bool advSuspended = false;
// BLE 栈的 deinit/init 必须与 NimBLEDevice::init 同任务（loop 任务）执行，
// 跨任务调用会崩（NimBLE 1.4.x 已知雷区）。AI 任务只发请求，slPoll 里真正干活。
static volatile bool bleSuspendReq = false;
static volatile bool bleResumeReq = false;
static SemaphoreHandle_t bleSuspendAck = nullptr;
static SemaphoreHandle_t bleResumeAck = nullptr;
static TaskHandle_t slLoopTask = nullptr;   // loop 任务句柄（slPoll 首次运行时记录，用于同任务检测）
static QueueHandle_t txQ = nullptr;
static uint32_t lastTxMs = 0;
static uint8_t tcpRxBuf[1024];
static slRxHook_t rxHook = nullptr;
static slCtrlHook_t ctrlHook = nullptr;
static uint16_t gConnHandle = 0xFFFF;   // 当前手机连接句柄（发送分片按对端 MTU 算）

// BLE 发送分片长度：按对端协商后的实际 MTU 算。
// 手机没请求 MTU 时默认 23 → 每包只能带 20 字节，notify 直接发 128 字节会被截断丢数据。
// APP 请求 MTU 247 后这里自动变成 244（上限仍是 TX_CHUNK=128 的队列项容量）。
static int bleChunkLen()
{
  if (gConnHandle == 0xFFFF) return TX_CHUNK;
  uint16_t mtu = ble_att_mtu(gConnHandle);
  if (mtu < 23) return 20;
  return min((int)TX_CHUNK, (int)mtu - 3);
}

void slSetRxHook(slRxHook_t fn) { rxHook = fn; }
void slSetCtrlHook(slCtrlHook_t fn) { ctrlHook = fn; }

class RxCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    NimBLEAttValue v = c->getValue();
    if (!v.length()) return;
    RxItem it;
    it.len = (uint16_t)min((int)v.length(), RX_MAX);
    memcpy(it.data, v.data(), it.len);
    xQueueSend(rxQ, &it, 0);
  }
};

class SrvCb : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *s) override { bleConnected = true; }
  // 浏览器/系统连上后默认常给 ~100ms+ 的连接间隔且每次事件只发一包，
  // 这是蓝牙 OTA 只有 ~2KB/s 的主因（240B × 8 次/秒 ≈ 2KB/s）。
  // 主动请求 15~30ms + 零从机延迟，速度可提 5~10 倍。
  void onConnect(NimBLEServer *s, ble_gap_conn_desc *desc) override
  {
    bleConnected = true;
    gConnHandle = desc->conn_handle;   // 记住连接句柄：发送分片按对端实际 MTU 算
    s->updateConnParams(desc->conn_handle,
                        12,    // 最小间隔 15ms（单位 1.25ms）
                        24,    // 最大间隔 30ms
                        0,     // 从机延迟
                        400);  // 监督超时 4s（单位 10ms）
  }
  void onDisconnect(NimBLEServer *s) override
  {
    bleConnected = false;
    gConnHandle = 0xFFFF;
    if (!bleDown) NimBLEDevice::startAdvertising();   // 挂起拆栈期间禁止重启广播
  }
};

#define SLOG(fmt, ...)                       \
  do                                         \
  {                                          \
    logTeePrintf(fmt, ##__VA_ARGS__);       \
    Serial0.printf(fmt, ##__VA_ARGS__);      \
  } while (0)

static void bleInitCore();

void slBegin(const char *name)
{
  snprintf(devName, sizeof(devName), "%s", name);
  SLOG("[ble] enter heap=%u int=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  rxQ = xQueueCreate(16, sizeof(RxItem));
  txQ = xQueueCreate(TX_QUEUE_N, sizeof(TxItem));
  SLOG("[ble] queue ok\n");
  bleSuspendAck = xSemaphoreCreateBinary();
  bleResumeAck = xSemaphoreCreateBinary();

  bleInitCore();

  tcpSrv = new WiFiServer(SL_TCP_PORT);
  tcpSrv->setNoDelay(true);
  SLOG("[ble] tcp ok\n");
}

// NimBLE 栈初始化：init + NUS 服务（队列/tcpSrv 不在这里，保证可重复调用）
// 注意：这里不启动广播，必须等所有 GATT 服务（NUS + 蓝牙 OTA）注册完再 slStartAdv()
// suspend 后库不再 delete 对象（见 NimBLEDevice::deinit 的 FIX 注释），resume 走复用路径：
// 幸存的 server/service 直接在新栈上重新 start 注册，不重建（重建会重复注册/泄漏）
static bool bleCoreBuilt = false;
static bool bleGattsDirty = false;   // resume 后置位：ATT 库需手动重建（见 slStartAdv）

static void bleInitCore()
{
  NimBLEDevice::init(devName);
  NimBLEDevice::setDeviceName(devName);
  SLOG("[ble] init ok\n");
  NimBLEDevice::setMTU(247);
  SLOG("[ble] mtu ok\n");
  bleSrv = NimBLEDevice::createServer();   // resume 时返回幸存的旧 server
  if (bleCoreBuilt)
  {
    // resume：host 是全新的，createServer 因旧对象幸存跳过了库表初始化，这里补上
    //（与首次启动 createServer 内部的三步一致，时序也相同：host 起来之后、注册服务之前）
    ble_gatts_reset();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    // port_stop 时 ble_gatts_stop 已清空服务列表和计数，这里重新 add 后需要重建 ATT 库；
    // boot 时这步由 adv->start → server->start() 完成，resume 时 server->start()
    // 因 m_gattsStarted 幸存而早退，必须由 slStartAdv 里的 bleGattsDirty 分支补上
    bleGattsDirty = true;
    SLOG("[ble] gatts reinit ok\n");
  }
  bleCoreBuilt = true;
  bleSrv->setCallbacks(new SrvCb());
  SLOG("[ble] server ok\n");
  NimBLEService *svc = bleSrv->getServiceByUUID(SVC_UUID);
  if (!svc)
  {
    // 首次构建 NUS 服务；resume 时对象幸存，直接复用
    svc = bleSrv->createService(SVC_UUID);
    txChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic *rxChar = svc->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rxChar->setCallbacks(new RxCb());
    SLOG("[ble] chars ok\n");
  }
  SLOG("[ble] service ok\n");
  svc->start();   // resume：把幸存的 service 重新注册到新栈（start 内部 count_cfg+add_svcs）
  SLOG("[ble] svc start ok\n");
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(devName);
  adv->setScanResponse(false);
  bleReady = true;
  SLOG("[ble] server ready, adv deferred\n");
}

// —— BLE 拆栈诊断插桩：每个子步骤打飞行记录仪存档点，崩溃重启后可读回，
// 用于锁定 NimBLEDevice::deinit 内部崩溃的具体语句。定位后可整体移除。——
// NimBLEDevice.cpp / nimble_port.c / esp_nimble_hci.c 中的插桩也调到这里（extern "C"）
extern "C" void bleDiagMark(int step, int rc)
{
  logTeePrintf("[ble] diag s%d rc=%d\n", step, rc);
  logTeePersistSave();
}

// TLS 握手前临时释放 BLE 内存（控制器+主机 ~50KB），握完 slBleResume 重建。
// 挂起期间网页蓝牙连接会断开，恢复后需重新连接。
// 实际挂起动作：只能在 loop 任务执行（slPoll 里调用），此处由请求/回执握手驱动
static void slBleSuspendImpl()
{
  if (bleDown) return;
  if (!bleReady) { bleDown = true; return; }
  // 标志必须先置位：deinit 拆连接会同步触发 onDisconnect 回调，回调里要跳过 BLE 操作
  bleDown = true;
  bleReady = false;
  SLOG("[ble] suspending heap=%u\n", (unsigned)ESP.getFreeHeap());
  logTeePersistSave();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (adv) adv->stop();
  bleDiagMark(10, 0);   // 广播已停，即将进入 deinit（s1=port_stop 返回）
  // 注意：txChar 等对象指针保持有效（对象不删除），resume 重注册后句柄自动更新
  NimBLEDevice::deinit(true);
  SLOG("[ble] suspended heap=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// AI 任务调用：请求挂起并等待 loop 任务完成（含 OTA 服务重注册 + 广播，见 slBleResume）
// 注意：拉模型等请求可能在 loop 任务上下文直接执行（设置页同步调用），此时走 req/ack
// 握手等于等自己，必然 3 秒超时——检测到同任务就同步直接执行。
bool slBleSuspend()
{
  if (bleDown) return true;
  if (!bleSuspendAck) return false;   // slBegin 未执行（不应发生）
  if (slLoopTask && xTaskGetCurrentTaskHandle() == slLoopTask)
  {
    slBleSuspendImpl();
    return bleDown;
  }
  xSemaphoreTake(bleSuspendAck, 0);   // 清残留
  bleSuspendReq = true;
  bool ok = xSemaphoreTake(bleSuspendAck, pdMS_TO_TICKS(3000)) == pdTRUE;
  if (!ok)
  { // 3 秒没等到回执：要么 loop 任务忙/卡住，要么已崩溃——用快照区分
    bleSuspendReq = false;   // 清残留：loop 迟到也不能再执行过期挂起
    logTeePrintf("[ble] suspend TIMEOUT\n");
    logTeePersistSave();
  }
  return ok && bleDown;
}

static void slBleResumeImpl()
{
  if (!bleDown) return;
  // 拆栈若损坏了堆，尽早暴露（heap_caps 会打印细节），避免在后面的分配里莫名崩
  if (!heap_caps_check_integrity_all(true))
    SLOG("[ble] HEAP CORRUPT after suspend!\n");
  bleDown = false;
  bleInitCore();
  SLOG("[ble] resumed heap=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  logTeePersistSave();
}

// AI 任务调用：请求重建并等待 loop 任务完成完整重建（栈 + 蓝牙 OTA 服务 + 广播）
// loop 任务上下文直接执行（同任务检测见 slBleSuspend，req/ack 会自锁）
void slBleResume()
{
  if (!bleDown) return;
  if (!bleResumeAck) return;
  if (slLoopTask && xTaskGetCurrentTaskHandle() == slLoopTask)
  {
    slBleResumeImpl();
    otaBleBegin();
    slStartAdv();
    return;
  }
  xSemaphoreTake(bleResumeAck, 0);
  bleResumeReq = true;
  xSemaphoreTake(bleResumeAck, pdMS_TO_TICKS(5000));
}

bool slBleDown() { return bleDown; }

// 所有 GATT 服务注册完之后再调用（见 otaBegin）
void slStartAdv()
{
  if (!bleReady) return;
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  if (bleGattsDirty)
  {
    // resume 后：服务已重新 add，但 server->start() 因 m_gattsStarted 幸存而早退，
    // ATT 库从未构建（现象=手机连上无可用 Service）。这里显式建库。
    // boot 时 bleGattsDirty=false，仍走 adv->start → server->start() 的正常建库路径。
    int grc = ble_gatts_start();
    bleGattsDirty = false;
    SLOG("[ble] gatts rebuild rc=%d\n", grc);
    logTeePersistSave();   // 建库也是高危步骤，崩在这要留现场
  }
  bool advOk = false;
  for (int i = 0; i < 50 && !advOk; i++)
  {
    advOk = adv->start();
    if (!advOk) vTaskDelay(pdMS_TO_TICKS(20));
  }
  SLOG("[ble] adv ok=%d active=%d\n", (int)advOk, (int)adv->isAdvertising());
  logTeePersistSave();
}

LinkMode slMode() { return mode; }
void slSetMode(LinkMode m) { mode = m; }
void slToggleMode() { mode = (mode == LINK_BLE) ? LINK_TCP : LINK_BLE; }

bool slActive()
{
  return (mode == LINK_BLE) ? bleConnected : tcpCli.connected();
}

String slLocalIp() { return WiFi.localIP().toString(); }

bool slBleConnected() { return bleConnected; }

const char *slDeviceName() { return devName; }

String slBleAddr()
{
  return String(NimBLEDevice::getAddress().toString().c_str());
}

bool slBleAdvActive()
{
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  return adv && adv->isAdvertising();
}

String slStatus()
{
  if (mode == LINK_BLE)
  {
    String st = bleConnected ? String("BLE 已连接 ") + devName : String("BLE 等待连接 ") + devName;
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    st += (adv && adv->isAdvertising()) ? " 广播中" : " 未广播";
    return st;
  }
  String s = "TCP 服务 " + slLocalIp() + ":" + String(SL_TCP_PORT);
  s += tcpCli.connected() ? " 已连接" : " 等待连接";
  return s;
}

bool slSend(const String &s)
{
  if (!s.length()) return false;
  return slSendRaw((const uint8_t *)s.c_str(), s.length());
}

bool slSendRaw(const uint8_t *d, int n)
{
  if (!d || n <= 0) return false;
  if (mode == LINK_BLE)
  {
    if (!bleConnected || !txChar || !txQ) return false;
    int chunk = bleChunkLen();
    // 只入队，由 slPoll() 每 10ms 送一片，避免 delay(20) 卡住主循环
    for (int i = 0; i < n; i += chunk)
    {
      TxItem it;
      it.len = (uint16_t)min(chunk, n - i);
      memcpy(it.data, d + i, it.len);
      if (xQueueSend(txQ, &it, 0) != pdTRUE) return i > 0;
    }
    return true;
  }
  if (!tcpCli.connected()) return false;
  return tcpCli.write(d, n) == (size_t)n;
}

void slOtaSuspend()
{
  advSuspended = true;
  if (!bleReady) return;
  NimBLEDevice::stopAdvertising();
}

void slOtaResume()
{
  advSuspended = false;
  if (!bleReady) return;
  NimBLEDevice::startAdvertising();
}

// BLE 发送分片：每 10ms 送一片，不阻塞主循环
static void serviceBleTx()
{
  if (!txQ) return;
  if (mode != LINK_BLE || !bleConnected || !txChar)
  {
    TxItem drop;
    while (xQueueReceive(txQ, &drop, 0) == pdTRUE) {}
    return;
  }
  uint32_t now = millis();
  if (now - lastTxMs < TX_GAP_MS) return;
  TxItem it;
  if (xQueueReceive(txQ, &it, 0) != pdTRUE) return;
  lastTxMs = now;
  txChar->setValue(it.data, it.len);
  txChar->notify();
}

void slPoll()
{
  if (!slLoopTask) slLoopTask = xTaskGetCurrentTaskHandle();

  // BLE 挂起/恢复请求：必须在 loop 任务（与 NimBLEDevice::init 同任务）执行真正的
  // deinit/init，跨任务调用会崩。恢复=完整重建（栈 + 蓝牙 OTA 服务 + 广播）。
  if (bleSuspendReq)
  {
    bleSuspendReq = false;
    slBleSuspendImpl();
    xSemaphoreGive(bleSuspendAck);
  }
  if (bleResumeReq)
  {
    bleResumeReq = false;
    slBleResumeImpl();
    otaBleBegin();
    slStartAdv();
    xSemaphoreGive(bleResumeAck);
  }

  serviceBleTx();

  // 广播自愈：连接断开、主机复位后广播可能丢，没连接时每 3 秒检查并重开
  static uint32_t lastAdv = 0;
  if (bleReady && !advSuspended && !bleConnected && millis() - lastAdv > 3000)
  {
    lastAdv = millis();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv && !adv->isAdvertising())
    {
      bool ok = adv->start();
      SLOG("[ble] adv retry ok=%d\n", (int)ok);
    }
  }

  if (rxQ)
  {
    RxItem it;
    while (xQueueReceive(rxQ, &it, 0) == pdTRUE)
    {
      if (ctrlHook && ctrlHook(it.data, it.len)) continue;   // APP 控制行：不记帧不透传
      if (mode == LINK_BLE) flAddRx(it.data, it.len);
      if (rxHook) rxHook(it.data, it.len);
    }
  }

  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !tcpListening)
  {
    tcpSrv->begin();
    tcpSrv->setNoDelay(true);
    tcpListening = true;
  }
  else if (!up && tcpListening)
  {
    tcpSrv->stop();
    tcpListening = false;
    tcpCli.stop();
  }

  if (tcpListening && !tcpCli.connected())
  {
    WiFiClient c = tcpSrv->accept();   // 3.x 里 available() 已改名 accept()
    if (c)
    {
      if (tcpCli && !tcpCli.connected()) tcpCli.stop();
      tcpCli = c;
    }
  }

  if (tcpCli.connected())
  {
    // 一次尽量读完整包，避免把一帧拆成多条记录
    int total = 0;
    while (total < (int)sizeof(tcpRxBuf) && tcpCli.available() > 0)
    {
      int n = tcpCli.read(tcpRxBuf + total, sizeof(tcpRxBuf) - total);
      if (n <= 0) break;
      total += n;
    }
    if (total > 0 && mode == LINK_TCP) flAddRx(tcpRxBuf, total);
  }
}
