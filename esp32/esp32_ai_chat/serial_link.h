#pragma once
#include <Arduino.h>

enum LinkMode : uint8_t { LINK_BLE, LINK_TCP };

#define SL_TCP_PORT 8888

void slBegin(const char *devName);
void slStartAdv();   // 所有 GATT 服务注册完之后调用，开始广播
void slPoll();
bool slActive();
LinkMode slMode();
void slSetMode(LinkMode m);
void slToggleMode();
String slStatus();
bool slBleConnected();
String slBleAddr();
bool slBleAdvActive();
const char *slDeviceName();
bool slSend(const String &s);
bool slSendRaw(const uint8_t *d, int n);   // 二进制转发到手机（蓝牙中继透传用，与 slSend 同队列）

// 收到手机/串口数据的钩子（slPoll 上下文，BLE/TCP 通用）：蓝牙中继反向透传在这里接
typedef void (*slRxHook_t)(const uint8_t *d, int n);
void slSetRxHook(slRxHook_t fn);
// 控制行钩子（仅 BLE 写路径，先于记帧/透传钩子）：返回 true=本包已消费（APP 控制行）
typedef bool (*slCtrlHook_t)(const uint8_t *d, int n);
void slSetCtrlHook(slCtrlHook_t fn);
String slLocalIp();
void slOtaSuspend();
void slOtaResume();
bool slBleSuspend();   // TLS 前临时释放 BLE 内存（网页蓝牙会断开）；实际 deinit 在 loop 任务执行
void slBleResume();    // 完整重建 BLE 栈（含 otaBleBegin + slStartAdv），loop 任务执行后返回
bool slBleDown();      // BLE 栈当前是否处于挂起状态