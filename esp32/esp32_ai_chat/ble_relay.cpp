#include "ble_relay.h"
#include "serial_link.h"
#include "frame_log.h"
#include "config_store.h"
#include "cfg_table.h"
#include "log_tee.h"
#include <NimBLEClient.h>
#include <NimBLEAdvertisedDevice.h>

// BT04E 透传服务的标准 UUID；连接后发现不了 FFE0 时退化为通用探测
// （任意可订阅 notify 的特征收、任意可写的特征发），换其他厂家模块也能用。
static const char *SVC_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb";
static const char *NOTIFY_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb";
static const char *WRITE_UUID = "0000ffe2-0000-1000-8000-00805f9b34fb";

static const uint32_t SCAN_MS = 4000;
static const uint32_t AUTO_RESCAN_MS = 8000;   // 自动重连的扫描周期

static String relayPref();   // 目标名称前缀（配置项 relay.name）

static NimBLEClient *client = nullptr;
static BLERemoteCharacteristic *notifyChar = nullptr;
static BLERemoteCharacteristic *writeChar = nullptr;
static String connDevName;

static volatile bool connectedFlag = false;
static bool wasDown = false;                    // BLE 栈重建自愈（同 bms_client）

// notify 回调在 BLE 主机线程，frames/slSend 都按 loop 上下文假设，
// 这里进小队列，relay::poll()（loop 任务）里统一分发。
struct NqItem { uint16_t len; uint8_t d[247]; };
static QueueHandle_t nq = nullptr;

// —— 扫描列表（BLE 回调线程更新，loop 线程读取；内含 String 用互斥锁）——
static std::vector<ScanItem> scanList;
static SemaphoreHandle_t listMux = nullptr;
static volatile bool scanRequested = false;
static uint32_t lastScanMs = 0;

static NimBLEAddress pendingAddr;
static volatile bool pendingConn = false;
static volatile bool connBusy = false;
static NimBLEAddress connTarget;
static void connTask(void *arg);
static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify);

class ClientCallbacks : public NimBLEClientCallbacks
{
  void onDisconnect(NimBLEClient *c) override
  {
    connectedFlag = false;
    notifyChar = nullptr;
    writeChar = nullptr;
    connDevName = "";
  }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  void onResult(NimBLEAdvertisedDevice *dev) override
  {
    ScanItem it;
    it.addr = dev->getAddress();
    it.rssi = dev->getRSSI();
    if (dev->haveName()) it.name = String(dev->getName().c_str());

    String pfx = relayPref();
    it.match = pfx.length() && it.name.startsWith(pfx);

    if (listMux) xSemaphoreTake(listMux, portMAX_DELAY);
    bool found = false;
    for (auto &e : scanList)
      if (e.addr == it.addr)
      {
        e.rssi = it.rssi;
        if (it.name.length()) e.name = it.name;
        e.match |= it.match;
        found = true;
        break;
      }
    if (!found && scanList.size() < 16) scanList.push_back(it);
    if (listMux) xSemaphoreGive(listMux);

    // 自动连接：开关开启时，名字命中前缀的设备直接连
    if (it.match && relayEnabled() && !connectedFlag && !pendingConn && !connBusy)
    {
      pendingAddr = it.addr;
      pendingConn = true;
    }
  }
};

static ClientCallbacks clientCb;
static ScanCallbacks scanCb;

static void onScanComplete(NimBLEScanResults) { scanRequested = false; }

static void startScan()
{
  if (slBleDown() || connectedFlag || connBusy || scanRequested) return;
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();   // 抢占 BMS 的后台扫描，结束后它会自行恢复
  if (listMux) xSemaphoreTake(listMux, portMAX_DELAY);
  scanList.clear();
  if (listMux) xSemaphoreGive(listMux);
  scan->setAdvertisedDeviceCallbacks(&scanCb);
  scan->setActiveScan(true);
  scan->start(SCAN_MS, onScanComplete, false);
  scanRequested = true;
  lastScanMs = millis();
}

void relayBegin()
{
  if (!listMux) listMux = xSemaphoreCreateMutex();
  if (!nq) nq = xQueueCreate(16, sizeof(NqItem));
  if (!client)
  {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCb);
    client->setConnectTimeout(8);
  }
  // 反向透传：手机发来的数据在透传开关打开时转发给模块
  slSetRxHook([](const uint8_t *d, int n) {
    if (relayPassOn() && relayConnected()) relaySend(d, n);
  });
  logTeePrintf("[relay] begin ok\n");
}

void relayPoll()
{
  if (slBleDown()) { wasDown = true; return; }
  if (wasDown)   // TLS 前挂起拆过栈：旧指针全部失效，重建并重新发现
  {
    wasDown = false;
    client = nullptr;
    connectedFlag = false;
    notifyChar = nullptr;
    writeChar = nullptr;
    connDevName = "";
    scanRequested = false;
    pendingConn = false;
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCb);
    client->setConnectTimeout(8);
  }
  if (!client) return;
  if (connBusy) return;

  // notify 数据分发（从 BLE 线程队列取，loop 里落帧记录 + 转发手机）
  if (nq)
  {
    NqItem it;
    while (xQueueReceive(nq, &it, 0) == pdTRUE)
    {
      flAddRx(it.d, it.len);
      slSendRaw(it.d, it.len);
    }
  }

  if (pendingConn && !connectedFlag)
  {
    pendingConn = false;
    connTarget = pendingAddr;
    if (NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
    connBusy = true;
    if (xTaskCreate(connTask, "rlconn", 6144, nullptr, 3, nullptr) != pdPASS)
      connBusy = false;
    return;
  }

  if (!relayEnabled())
  {
    if (connectedFlag) client->disconnect();   // 关开关即断开
    return;
  }

  // 自动重连：开着开关且未连接 → 周期性短扫描找目标模块
  if (!connectedFlag && !scanRequested &&
      !NimBLEDevice::getScan()->isScanning() &&
      millis() - lastScanMs > AUTO_RESCAN_MS)
    startScan();
}

static void connectToServer(const NimBLEAddress &addr)
{
  if (!client->connect(addr))
  {
    logTeePrintf("[relay] connect fail\n");
    return;
  }
  BLERemoteService *svc = client->getService(SVC_UUID);
  if (svc)
  {
    notifyChar = svc->getCharacteristic(NOTIFY_UUID);
    writeChar = svc->getCharacteristic(WRITE_UUID);
  }
  else
  {
    // 通用探测：遍历服务找 notify + write 特征（FFE0 找不到时兜底）
    logTeePrintf("[relay] FFE0 missing, probe all\n");
    for (auto *s : *client->getServices())
    {
      if (!s) continue;
      for (auto *ch : *s->getCharacteristics())
      {
        if (!ch) continue;
        if (!notifyChar && ch->canNotify()) notifyChar = ch;
        if (!writeChar && (ch->canWrite() || ch->canWriteNoResponse())) writeChar = ch;
      }
    }
  }
  if (!notifyChar)
  {
    logTeePrintf("[relay] no notify char, drop\n");
    client->disconnect();
    notifyChar = nullptr;
    writeChar = nullptr;
    return;
  }
  notifyChar->subscribe(true, onNotify);
  connectedFlag = true;
  connDevName = String(addr.toString().c_str());
  logTeePrintf("[relay] connected, notify=%s write=%s\n",
               notifyChar->getUUID().toString().c_str(),
               writeChar ? writeChar->getUUID().toString().c_str() : "none");
}

static void connTask(void *arg)
{
  connectToServer(connTarget);
  connBusy = false;
  vTaskDelete(nullptr);
}

static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify)
{
  if (!nq || len <= 0) return;
  NqItem it;
  it.len = (uint16_t)min((int)len, 247);
  memcpy(it.d, data, it.len);
  xQueueSend(nq, &it, 0);
}

bool relayEnabled()
{
  int i = cfgIndexOf("relay.on");
  return i >= 0 && cfgGetRaw(cfgAt(i)) == "1";
}

bool relayPassOn()
{
  int i = cfgIndexOf("relay.pass");
  return i >= 0 && cfgGetRaw(cfgAt(i)) == "1";
}

static String relayPref()
{
  int i = cfgIndexOf("relay.name");
  String p = i >= 0 ? cfgGetRaw(cfgAt(i)) : String("BT04E");
  p.trim();
  return p;
}

bool relayConnected() { return connectedFlag; }
bool relayConnecting() { return connBusy; }
bool relayScanActive() { return scanRequested; }
String relayConnName() { return connDevName; }

int relayScanCount()
{
  if (!listMux) return 0;
  xSemaphoreTake(listMux, portMAX_DELAY);
  int n = (int)scanList.size();
  xSemaphoreGive(listMux);
  return n;
}

ScanItem relayScanAt(int i)
{
  ScanItem it;
  if (listMux) xSemaphoreTake(listMux, portMAX_DELAY);
  if (i >= 0 && i < (int)scanList.size()) it = scanList[i];
  if (listMux) xSemaphoreGive(listMux);
  return it;
}

void relayRescan()
{
  if (slBleDown() || connBusy) return;
  if (connectedFlag) { logTeePrintf("[relay] 已连接，先断开再扫描\n"); return; }
  startScan();
}

bool relayConnectAddr(const NimBLEAddress &a)
{
  if (connectedFlag || pendingConn || connBusy || slBleDown()) return false;
  pendingAddr = a;
  pendingConn = true;
  return true;
}

void relayDisconnect()
{
  if (client && connectedFlag) client->disconnect();
}

bool relaySend(const uint8_t *d, int n)
{
  if (!writeChar || !connectedFlag || n <= 0) return false;
  return writeChar->writeValue((uint8_t *)d, n, false);   // 无应答写，吞吐优先
}
