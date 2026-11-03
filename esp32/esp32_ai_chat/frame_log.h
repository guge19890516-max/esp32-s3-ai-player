#pragma once
#include <Arduino.h>
#include <vector>

#define FL_MAX_FRAMES 60
#define FL_TEXT_MAX  400

struct FrameRec
{
  bool rx;
  uint32_t ms;
  int len;
  String hex;
  String text;
};

extern std::vector<FrameRec> frames;
extern bool flHexMode;

// 帧旁路钩子：每帧（含原始字节）在入列表后回调一次，log_store 落盘 / 规则引擎解码在这里接
typedef void (*flTap_t)(bool rx, uint32_t ms, const uint8_t *d, int n);
void flSetTap(flTap_t fn);

void flBegin();
void flAddRx(const uint8_t *d, int n);
void flAddRxText(const String &s);
void flAddTx(const String &s);
void flAddTxRaw(const uint8_t *d, int n);
void flAddTxWire(const String &utf8Text, const uint8_t *wire, int n);   // hex=链路字节，text=原输入
void flClear();
void flToggleHex();
String flHexOf(const uint8_t *d, int n);
String flTextOf(const uint8_t *d, int n);
String flTime(uint32_t ms);