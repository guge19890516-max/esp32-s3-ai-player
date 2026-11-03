#pragma once
#include <Arduino.h>

// TF 卡落盘（docs/app-protocol.md 第 6 节）：
//   /logs/<会话>/frames.jsonl   每帧一行 {"t":ms,"rx":0/1,"len":n,"hex":"AA BB"}
//   /logs/<会话>/decoded.csv    解析结果（cfg 项 dec.csv 开启时）
// 会话：中继连上自动开 / 断开收尾，APP 也可 log.start / log.stop 手动控制；
// 会话名 log_MMDD_HHMM（时间未同步时用运行秒数兜底）；单文件 2MB 自动分卷。
// 写入走 RAM 队列，lsPoll()（loop 里）批量刷盘，不卡主循环。

void lsBegin();                    // 空探测（真正的挂载在 lsStart / lsEnsureMount 时做）
bool lsEnsureMount();              // 挂载 TF（幂等）；规则文件的读写也复用这里
bool lsMounted();
bool lsStart(const char *reason);  // 开会话（无卡返回 false）
bool lsStop();                     // 收尾：flush + 关文件（挂载保留，便于快速重开）
bool lsActive();
String lsSession();                // 会话目录名（未开会话返回空）
void lsFrame(bool rx, uint32_t ms, const uint8_t *d, int n);  // 入 RAM 队列，lsPoll 落盘
void lsDecoded(uint32_t ms, int dev, const String &csvVals);  // decoded.csv（内部判开关）
void lsPoll();                     // loop 里调用：批量刷盘
String lsStatus();                 // 一行状态：会话名 + 已写字节
