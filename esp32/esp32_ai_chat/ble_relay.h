#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// 蓝牙中继：ESP32 作主机连接蓝牙串口模块（BT04E 等），把模块收到的报文
// 转发到屏幕帧记录 + 手机（NUS 透传）；反向（手机→模块）走 FFE2，由
// serial_link 的 RX 钩子在「串口透传」开关打开时转发。

struct ScanItem
{
  NimBLEAddress addr;
  String name;
  int rssi = -100;
  bool match = false;   // 名称命中配置的前缀（大概率是目标模块）
};

void relayBegin();       // NimBLE 就绪后调用（slBegin 之后）
void relayPoll();        // loop 里调用：扫描调度/自动重连/连接任务/notify 分发

bool relayEnabled();     // 中继总开关（配置项 relay.on）
bool relayPassOn();      // 反向透传开关（配置项 relay.pass）
bool relayConnected();
bool relayConnecting();
bool relayScanActive();
String relayConnName();  // 已连接设备名（未连接返回空）

int relayScanCount();
ScanItem relayScanAt(int i);
void relayRescan();      // 手动重扫（中继页按钮）
bool relayConnectAddr(const NimBLEAddress &a);   // 点选连接（异步）
void relayDisconnect();

bool relaySend(const uint8_t *d, int n);      // 手机数据 → 模块 FFE2（透传）
