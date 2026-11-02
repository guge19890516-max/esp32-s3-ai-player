#pragma once
#include <Arduino.h>

// GBK<->Unicode 转换表（tools/gen_gbk_table.py 生成，const 数组在 flash）
uint16_t uniToGbk(uint16_t u);   // Unicode -> GBK 双字节码点，0=不可映射
uint16_t gbkToUni(uint16_t idx); // 表索引(见 text_codec.cpp) -> Unicode 码点
