#pragma once
#include <Arduino.h>

// NUS 控制行协议（docs/app-protocol.md 第 3 节）：
// 以 { 开头、\n 结尾的单行 JSON 命令（8KB 上限，支持跨包拼行）；
// 其余字节原样透传给蓝牙模块（relayPassThrough）。
// acInit() 必须在 relayBegin() 之后调用（接管 slSetRxHook）。

void acInit();
