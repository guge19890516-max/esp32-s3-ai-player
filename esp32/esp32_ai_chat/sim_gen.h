#pragma once
#include <Arduino.h>

// 内置模拟测试帧（联调辅助），两种帧独立开关：
//
// 1. 开关量帧（sim.io / sim.on / sim.off）：Modbus RTU 读线圈应答风格，
//    路数可配（sim.cnt 1~64，默认 8），模式（sim.mode）：随机=每帧随机翻转 0~2 路，
//    固定=每帧发 sim.mask 指定的位图。帧：01 03 <byteCount> <位图...> CRC_LO CRC_HI。
//    位图 hex 高字节在前，bit0=路1。byteCount = (路数+7)/8。
//
// 2. 遥测帧（sim.te / te.on / te.off）：Modbus RTU 读输入寄存器应答（功能码 04），
//    4 通道 u16 大端：电压(0.1V)/电流(0.1A)/功率(1W)/温度(0.1℃)，值带随机波动。
//    帧：01 04 08 <电压> <电流> <功率> <温度> CRC_LO CRC_HI（13 字节）。
//
// 走完整链路：flAddRx（上屏/落盘/规则解码）+ slSendRaw（透传字节发手机）。
// 配置页改动即时生效（发帧时从 NVS 读参数）。

namespace sim {

void begin();        // 读 cfg 恢复开关状态
void poll();         // loop 里调用
void set(bool on);   // 开关量帧
void setTe(bool on); // 遥测帧
bool on();
bool teOn();

}  // namespace sim
