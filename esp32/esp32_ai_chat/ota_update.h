#pragma once
#include <Arduino.h>

#define FW_VERSION "1.3.1"

enum OtaState : uint8_t { OST_IDLE, OST_RUNNING, OST_DONE, OST_FAIL };

void otaBegin();
void otaPoll();
OtaState otaState();
int otaPercent();
String otaMessage();
String otaVersion();
String otaSlot();
String otaFreeSpace();
bool otaArmed();
void otaSetArmed(bool a);
bool otaWebStart(const String &url);
bool otaBusy();
bool otaRestartPending();

// SD card update: look for /update/fw.bin on the TF card.
// Returns true only if a firmware was flashed (device restarts, so normally never returns).
bool otaSdUpdate();

// BLE OTA service (Web Bluetooth). Call after serial_link / NimBLE server is up.
void otaBleBegin();
bool otaBleReady();
bool otaBleActive();   // 蓝牙正在接收固件
void otaBleSetInfo(const String &s);   // 更新蓝牙"信息"特征（网页据此自动填 IP）

// 供 HTTP 推送通道复用
void otaReport(OtaState s, const String &msg, int pct);
void otaRequestRestart(uint32_t delayMs);
size_t otaTargetSize();