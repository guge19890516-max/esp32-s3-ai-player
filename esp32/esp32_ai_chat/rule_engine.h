#pragma once
#include <Arduino.h>
#include <vector>

// 协议规则引擎：按 APP 下发的规则（docs/app-protocol.md 第 4 节 Schema）实时解码报文。
// 有 addr = 多设备：按帧内设备地址分组维护当前值，evt:dec 带 dev；
// 单设备不带 dev。规则持久化在 /rules/<名称>.json，重启后自动恢复上次激活的规则。
// 解码只处理 RX（总线 → 设备的应答帧）。

namespace re {

enum FldType : uint8_t { FT_U8, FT_I8, FT_U16, FT_I16, FT_U32, FT_I32, FT_F32, FT_BIT };

struct Field
{
  String name;
  uint16_t off = 0;
  uint8_t type = FT_U16;
  bool be = false;       // 1=大端（u16/u32 有效）
  uint8_t bit = 0;       // 仅 FT_BIT：位偏移 0-7
  float scale = 1.0f;
  String unit;
  std::vector<std::pair<int32_t, String>> enums;   // 原始整数值 → 显示文本
};

struct Rule
{
  String name;
  int ver = 1;
  String src;
  bool multi = false;
  uint16_t addrOff = 0;
  uint8_t addrType = FT_U8;   // 仅 u8 / u16
  bool addrBe = false;
  struct Match { uint16_t off; std::vector<uint8_t> vals; };
  std::vector<Match> match;
  std::vector<Field> fields;
};

struct DecResult
{
  bool ok = false;
  int dev = 0;        // 设备地址（单设备 = 0）
  String line;        // 人话一行："电压 220.5V 电流 1.2A"
  String csvVals;     // decoded.csv 值列："220.5","1.2"（每值带引号，逗号拼接）
};

void reBegin();                                     // 从 TF 恢复上次激活的规则
bool setRuleJson(const String &json, String &err);  // rule.set：解析 + 生效 + 存 TF
bool getRuleJson(const String &name, String &out);  // rule.get（不存在返回 false）
bool delRule(const String &name);
void listNames(std::vector<String> &out);           // /rules/*.json + 当前激活规则
bool hasRule();
String ruleName();
void setPush(bool on);                              // evt:dec 推送开关（dec.on/off）
bool pushOn();

void tapFrame(bool rx, uint32_t ms, const uint8_t *d, int n, DecResult &out);

// 屏幕解码页（S_DECODE）
int devCount();                    // 当前有值的设备数
int devAddrAt(int i);
int fieldCount();
const Field &fieldAt(int i);
String curVal(int dev, int fieldIdx);   // 当前值文本（未解析到 = 空）
uint32_t curMs(int dev);                // 该设备最后更新时刻（millis）

}  // namespace re
