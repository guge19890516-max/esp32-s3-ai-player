#include "bms_client.h"
#include "serial_link.h"
#include "ble_relay.h"
#include <NimBLEClient.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEUtils.h>

namespace bms {

static const char *SVC_UUID = "0000ff00-0000-1000-8000-00805f9b34fb";
static const char *NOTIFY_UUID = "0000ff01-0000-1000-8000-00805f9b34fb";
static const char *WRITE_UUID = "0000ff02-0000-1000-8000-00805f9b34fb";

static const uint8_t FRAME_START = 0xDD;
static const uint8_t FRAME_END = 0x77;
static const uint8_t MODE_READ = 0xA5;
static const uint8_t CMD_BASE_INFO = 0x03;
static const uint8_t CMD_CELL_VOLTAGE = 0x04;

static const uint32_t POLL_INTERVAL_MS = 2000;
static const uint32_t SCAN_DURATION_MS = 5000;

static NimBLEClient *client = nullptr;
static BLERemoteCharacteristic *notifyChar = nullptr;
static BLERemoteCharacteristic *writeChar = nullptr;

static bool connectedFlag = false;
static bool hasDataFlag = false;
static BaseInfo lastInfo;
static uint32_t lastPoll = 0;
static uint8_t pendingCmd = 0;
static bool writeInFlight = false;

static uint8_t streamBuf[2048];
static int streamLen = 0;

static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify);
static void connectToServer(const NimBLEAddress &addr);
static void buildRequest(uint8_t cmd, uint8_t *out, int *outLen);
static bool verifyChecksum(uint8_t *frame, int total);
static void handleFrame(uint8_t *frame, int total);
static void parseBaseInfo(uint8_t *c, int len);
static void pump();

class ClientCallbacks : public NimBLEClientCallbacks
{
  void onConnect(NimBLEClient *c) override
  {
    connectedFlag = true;
  }
  void onDisconnect(NimBLEClient *c) override
  {
    connectedFlag = false;
    hasDataFlag = false;
    notifyChar = nullptr;
    writeChar = nullptr;
  }
};

// —— fix14：可见扫描列表。很多电动车 BMS 广播里不带 FF00 服务 UUID（只在连接后的
// GATT 表里才有），旧逻辑 isAdvertisingService(FF00) 一票否决 → "永远扫描中"。
// 现在收一切广播进列表，用户能点选任意设备尝试连接；FF00 设备仍自动连。——
static std::vector<ScanItem> scanList;
static SemaphoreHandle_t listMux = nullptr;   // BLE host 回调与 loop 线程共享列表（内含 String，不能用自旋锁）
static NimBLEAddress pendingAddr;      // 待连接目标（poll 上下文执行）
static volatile bool pendingConn = false;
static volatile bool pendingAuto = false;   // 自动连（FF00）还是用户点选
static NimBLEAddress connTarget;      // 连接任务的目标地址
static volatile bool connBusy = false;   // 连接任务运行中（阻塞式 connect 移出 loop，UI 不再冻结 30 秒）
static void connTask(void *arg);

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  void onResult(NimBLEAdvertisedDevice *dev) override
  {
    ScanItem it;
    it.addr = dev->getAddress();
    it.rssi = dev->getRSSI();
    if (dev->haveName()) it.name = String(dev->getName().c_str());
    it.hasFF00 = dev->haveServiceUUID() && dev->isAdvertisingService(NimBLEUUID(SVC_UUID));

    if (listMux) xSemaphoreTake(listMux, portMAX_DELAY);
    bool found = false;
    for (auto &e : scanList)
      if (e.addr == it.addr) { e.rssi = it.rssi; if (it.name.length()) e.name = it.name; e.hasFF00 |= it.hasFF00; found = true; break; }
    if (!found && scanList.size() < 16) scanList.push_back(it);
    if (listMux) xSemaphoreGive(listMux);

    if (it.hasFF00 && !connectedFlag && !pendingConn)
    {
      pendingAddr = it.addr;
      pendingAuto = true;
      pendingConn = true;   // 连接动作在 poll() 主循环执行，不在 BLE 回调里做阻塞操作
    }
  }
};

static ClientCallbacks clientCb;
static ScanCallbacks scanCb;
static volatile bool scanRequested = false;

static void onScanComplete(NimBLEScanResults results)
{
  scanRequested = false;
}

bool begin()
{
  NimBLEDevice::init("ESP32-BMS");
  NimBLEDevice::setMTU(247);
  if (!listMux) listMux = xSemaphoreCreateMutex();
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCb);
  client->setConnectTimeout(8);   // 默认 30s：点选不可连接设备会把 loop 卡死 30 秒
  return true;
}

static bool wasDown = false;

void poll()
{
  if (slBleDown()) { wasDown = true; return; }   // TLS 握手期间 BLE 栈已释放，避免踩空指针
  if (wasDown)   // BLE 栈刚重建：旧 client/特征指针已随旧栈释放，重置后自动重连
  {
    wasDown = false;
    client = nullptr;
    connectedFlag = false;
    hasDataFlag = false;
    notifyChar = nullptr;
    writeChar = nullptr;
    scanRequested = false;
    pendingConn = false;
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCb);
  }
  if (!client) return;

  // fix14b：连接任务运行期间挂起一切扫描/轮询，避免与阻塞式 connect 并发
  if (connBusy) return;

  // fix14：连接请求（FF00 自动匹配或用户点选）——独立任务执行，loop 不再阻塞 30 秒
  if (pendingConn && !connectedFlag)
  {
    pendingConn = false;
    connTarget = pendingAddr;
    if (NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
    connBusy = true;
    if (xTaskCreate(connTask, "bmsconn", 6144, nullptr, 3, nullptr) != pdPASS)
      connBusy = false;   // 任务创建失败则放弃本次连接，下一轮恢复扫描
    return;
  }

  if (!connectedFlag)
  {
    if (relayScanActive()) return;   // 蓝牙中继正在扫描：让出扫描器，中继结束后自动恢复
    if (NimBLEDevice::getScan()->isScanning()) return;
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCb);
    scan->setActiveScan(true);   // 取 scan response 里的完整设备名
    scan->start(SCAN_DURATION_MS, onScanComplete, false);
    return;
  }

  if (millis() - lastPoll < POLL_INTERVAL_MS) return;
  lastPoll = millis();

  if (!writeChar) return;
  if (writeInFlight) return;

  pendingCmd = CMD_BASE_INFO;
  pump();
}

static void pump()
{
  if (!writeChar || writeInFlight) return;
  uint8_t frame[16];
  int len = 0;
  buildRequest(pendingCmd, frame, &len);
  writeChar->writeValue(frame, len, true);
  writeInFlight = true;
}

static void buildRequest(uint8_t cmd, uint8_t *out, int *outLen)
{
  out[0] = FRAME_START;
  out[1] = MODE_READ;
  out[2] = cmd;
  out[3] = 0;
  int sum = cmd;
  int chk = (~sum + 1) & 0xFFFF;
  out[4] = (chk >> 8) & 0xFF;
  out[5] = chk & 0xFF;
  out[6] = FRAME_END;
  *outLen = 7;
}

static void connectToServer(const NimBLEAddress &addr)
{
  if (!client->connect(addr))
    return;   // 失败后 poll 下一轮自动恢复扫描

  BLERemoteService *svc = client->getService(SVC_UUID);
  if (!svc)
  {
    client->disconnect();
    return;
  }
  notifyChar = svc->getCharacteristic(NOTIFY_UUID);
  writeChar = svc->getCharacteristic(WRITE_UUID);
  if (!notifyChar || !writeChar)
  {
    client->disconnect();
    return;
  }
  notifyChar->subscribe(true, onNotify);
}

// 连接在独立任务里做阻塞式 connect/GATT 发现，loop 只负责 UI 与扫描调度
static void connTask(void *arg)
{
  connectToServer(connTarget);
  connBusy = false;
  vTaskDelete(nullptr);
}

static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify)
{
  writeInFlight = false;
  if (len <= 0) return;
  if (streamLen + len > (int)sizeof(streamBuf)) streamLen = 0;
  memcpy(streamBuf + streamLen, data, len);
  streamLen += len;

  int i = 0;
  while (i < streamLen)
  {
    if (streamBuf[i] != FRAME_START) { i++; continue; }
    if (streamLen - i < 4) break;
    int total = (streamBuf[i + 3] & 0xFF) + 7;
    if (streamLen - i < total) break;
    if (streamBuf[i + total - 1] == FRAME_END && verifyChecksum(streamBuf + i, total))
    {
      handleFrame(streamBuf + i, total);
      i += total;
    }
    else i++;
  }
  if (i > 0)
  {
    memmove(streamBuf, streamBuf + i, streamLen - i);
    streamLen -= i;
  }
}

static bool verifyChecksum(uint8_t *frame, int total)
{
  int len = frame[3] & 0xFF;
  int sum = len + (frame[2] & 0xFF);
  for (int i = 0; i < len; i++) sum += frame[4 + i] & 0xFF;
  int expected = (~sum + 1) & 0xFFFF;
  return frame[total - 3] == (expected >> 8) && frame[total - 2] == (expected & 0xFF);
}

static void handleFrame(uint8_t *frame, int total)
{
  int len = frame[3] & 0xFF;
  uint8_t *content = frame + 4;
  int status = frame[2] & 0xFF;
  if (status != 0) return;

  int echoed = frame[1] & 0xFF;
  if (echoed == CMD_BASE_INFO) parseBaseInfo(content, len);
  else if (echoed == CMD_CELL_VOLTAGE) { /* cell voltages parsed on demand */ }
}

static void parseBaseInfo(uint8_t *c, int len)
{
  if (len < 23) return;
  BaseInfo &I = lastInfo;
  I.totalVoltage = ((c[0] << 8) | c[1]) / 100.0f;
  I.current = (int16_t)((c[2] << 8) | c[3]) / 100.0f;
  I.remainPower = ((c[4] << 8) | c[5]) / 100.0f;
  I.nominalPower = ((c[6] << 8) | c[7]) / 100.0f;
  I.cycles = (c[8] << 8) | c[9];
  I.soc = c[19] & 0xFF;
  I.cellCount = c[21] & 0xFF;
  I.ntcCount = c[22] & 0xFF;
  for (int i = 0; i < I.ntcCount && i < 4; i++)
  {
    int idx = 23 + i * 2;
    if (idx + 1 < len)
      I.temperatures[i] = (((c[idx] << 8) | c[idx + 1]) - 2731) / 10.0f;
  }
  int tail = I.ntcCount * 2 + 22;
  if (len > tail + 5)
  {
    I.extended = true;
    I.balanceCurrent = ((c[tail + 4] << 8) | c[tail + 5]) / 100.0f;
  }
  hasDataFlag = true;
}

bool connected() { return connectedFlag; }
bool hasData() { return hasDataFlag; }
bool connecting() { return connBusy; }
BaseInfo info() { return lastInfo; }

int scanCount()
{
  if (!listMux) return 0;
  xSemaphoreTake(listMux, portMAX_DELAY);
  int n = (int)scanList.size();
  xSemaphoreGive(listMux);
  return n;
}

ScanItem scanAt(int i)
{
  ScanItem it;
  if (listMux) xSemaphoreTake(listMux, portMAX_DELAY);
  if (i >= 0 && i < (int)scanList.size()) it = scanList[i];
  if (listMux) xSemaphoreGive(listMux);
  return it;
}

void rescan()
{
  if (!listMux) return;
  xSemaphoreTake(listMux, portMAX_DELAY);
  scanList.clear();
  xSemaphoreGive(listMux);
  if (!slBleDown() && !connectedFlag && !connBusy && !NimBLEDevice::getScan()->isScanning())
  {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCb);
    scan->setActiveScan(true);
    scan->start(SCAN_DURATION_MS, onScanComplete, false);
  }
}

bool connectAddr(const NimBLEAddress &a)
{
  if (connectedFlag || pendingConn || connBusy || slBleDown()) return false;
  pendingAddr = a;
  pendingAuto = false;
  pendingConn = true;
  return true;
}

}
