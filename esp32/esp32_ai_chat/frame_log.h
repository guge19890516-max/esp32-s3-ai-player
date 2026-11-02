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