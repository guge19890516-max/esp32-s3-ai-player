#pragma once
#include <Arduino.h>

// 串口收发编码转换（编码名与配置项取值一致：UTF-8/ASCII/GBK/GB2312/UNICODE）
// 发送：UTF-8 输入 -> 发送编码字节串；接收显示：接收编码字节 -> UTF-8
// GB2312 是 GBK 子集，共用一张表；UNICODE = UTF-16LE（BOM 由配置项决定）

String tcEncodeTx(const String &utf8);          // 按配置的发送编码转换
String tcDecodeRx(const uint8_t *d, int n);     // 按配置的接收编码转成 UTF-8 用于显示
String tcUtf8ToGbk(const String &utf8);         // 显式 GBK（含 GB2312）
String tcGbkToUtf8(const uint8_t *d, int n);
String tcUtf8ToUni(const String &utf8, bool bom);   // UTF-16LE
String tcUniToUtf8(const uint8_t *d, int n);
void tcUtf8Put(String &s, uint32_t cp);         // 码点 -> UTF-8 追加
