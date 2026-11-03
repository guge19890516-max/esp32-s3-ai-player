#pragma once
#include <Arduino.h>

// 内置模拟测试帧（联调辅助）：
// 周期生成 Modbus RTU 风格的 8 路开关量帧（读线圈应答），状态随机翻转，
// 走完整链路：flAddRx（上屏/落盘/规则解码）+ slSendRaw（透传字节发手机）。
// 帧格式：01 03 01 <状态字节> CRC_LO CRC_HI（6 字节，CRC16-Modbus 真实计算，低字节在前）。
// 开启：控制行 sim.on / sim.off，或配置项「模拟开关量帧」。

namespace sim {

void begin();        // 读 cfg sim.io 恢复状态
void poll();         // loop 里调用
void set(bool on);
bool on();

}  // namespace sim
