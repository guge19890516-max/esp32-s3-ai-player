#pragma once
#include <Arduino.h>

// 内置模拟测试帧（联调辅助），两种帧独立开关：
//
// 1. 开关量帧（sim.io / sim.on / sim.off）：Modbus RTU 读线圈应答风格，
//    路数可配（sim.cnt 1~64，默认 8），模式（sim.mode）：随机=每帧随机翻转 0~2 路，
//    固定=发当前位图。帧：01 03 <byteCount> <位图...> CRC_LO CRC_HI。
//    位图 hex 高字节在前，bit0=路1。byteCount = (路数+7)/8。
//    当前位图 gState 由屏幕按钮页直接编辑并同步回 sim.mask 配置项。
//
// 2. 遥测帧（sim.te / te.on / te.off）：Modbus RTU 读输入寄存器应答（功能码 04），
//    通道数可配（te.cnt 1~8，默认 4），每通道基准值可配（te.ch1~te.ch8，u16 大端），
//    波动幅度 = 基准 ±1%（至少 ±1）。帧：01 04 <2N> <通道值...> CRC_LO CRC_HI。
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
uint64_t bits();           // 开关量当前位图（bit0=路1）
void setBits(uint64_t v);  // 屏幕按钮编辑位图（调用方负责同步回 sim.mask 配置）

}  // namespace sim
