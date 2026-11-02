#pragma once
#include <Arduino.h>

// 设备配置表：整个固件里唯一一处定义"有哪些设置项"。
// 加一项设置 = 在 cfg_table.cpp 的 TABLE[] 里加一行，屏幕设置页和网页设置页都会自动出现。

enum CfgType : uint8_t
{
  CT_STR = 0,
  CT_INT,
  CT_BOOL,
  CT_ENUM
};

#define CFG_SECRET 0x01   // 读接口里打码显示
#define CFG_RESTART 0x02  // 改动后提示需重启

struct CfgItem
{
  const char *key;     // NVS 键，≤15 字符
  const char *group;   // 分组名（表内必须连续排列）
  const char *label;   // 显示名
  uint8_t type;
  int32_t minv, maxv;  // CT_INT 的范围
  const char *options; // CT_ENUM 的选项，竖线分隔："a|b|c"
  uint8_t flags;
  const char *defval;
};

void cfgInit();                        // 生成 PIN、准备存储

int cfgCount();
const CfgItem &cfgAt(int i);

String cfgGetRaw(const CfgItem &it);   // 真实值
String cfgGetShow(const CfgItem &it);  // secret 项打码
bool cfgSet(const CfgItem &it, const String &v, String &err);

// 供 HTTP / BLE 接口使用
void cfgBuildSettingsJson(String &out);
void cfgBuildStatusJson(String &out);

bool cfgWebPinRequired();
uint16_t cfgPin();                     // 本次开机随机 4 位

String cfgJsonEsc(const String &s);    // JSON 字符串转义（固件内部拼 JSON 共用）

// AI 厂商：当前厂商名 / 该厂商已拉取的模型列表（'\n' 分隔）
String cfgVendorName();
String cfgModels();                    // 运行时缓存，来自“拉取模型”或切厂商时从记录恢复
void cfgSetModels(const String &models);
int  cfgIndexOf(const char *key);      // 按 key 找表下标，找不到 -1
bool cfgFlag(const char *key);         // BOOL 项当前值（"1"/"0"）

// 串口编码（内存缓存，收发路径每帧都要查）：UTF-8/ASCII/GBK/GB2312/UNICODE
String cfgTxEnc();                     // 发送编码
String cfgRxEnc();                     // 接收编码
bool cfgUniBom();                      // UNICODE 是否带 BOM (FF FE)
