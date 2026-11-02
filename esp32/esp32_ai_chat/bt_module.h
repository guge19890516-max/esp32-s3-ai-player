#pragma once
#include <Arduino.h>

// BT04E 蓝牙串口模块的 AT 配置引擎：用一路 UART（UART2）按需开关连接模块的 TTL 串口，
// 会话结束即 end()，不常驻，也不与数据链路（BLE/TCP 中继）冲突。
// 引脚与会话波特率来自 cfg_table 的 btmod.tx / btmod.rx / btmod.baud（屏幕配置项与网页均可改）。
// 注意：BT04E 的 AT 命令仅在模块无蓝牙连接时有效。

String btmodCmd(const String &cmd, uint32_t timeoutMs = 900); // 发一条 AT 命令，收响应直到 OK/ERROR/超时
bool btmodSet(const String &cmd);                             // 设置类命令：发送并验证响应含 OK
String btmodInfo();                                           // 查版本/MAC/名称/波特率/校验/停止位，拼成 JSON
bool btmodNeedsReset(const String &cmd);                      // 是否为设置类命令（成功后应补发 AT+RESET）

// —— AT 口原始收发调试（验证 GPIO41/42 串口链路本身；打开期间 AT 会话不可用）——
bool btmodDebug(bool on);                                     // 打开/关闭调试口（引脚/波特率取配置项）
bool btmodDebugOn();                                          // 当前是否处于调试模式
size_t btmodDebugSend(const String &data, bool hexMode, String &err); // 发原始字节（hexMode 时按 HEX 解析）
String btmodDebugRecvHex();                                   // 取走收到的字节（HEX 空格分隔），取后清空
uint32_t btmodDebugRxCount();                                 // 本次调试累计收到的字节数
void btmodDebugPump();                                        // 主循环轻量收取 RX，防硬件 FIFO 溢出
