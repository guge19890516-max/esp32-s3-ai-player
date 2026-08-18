#pragma once
#include <Arduino.h>
#include <Wire.h>

struct TouchPoint {
    int16_t x;
    int16_t y;
    bool pressed;
};

class TouchDriver {
public:
    bool begin();
    TouchPoint read();
    bool isTouched();

private:
    uint8_t _readReg(uint8_t reg);
    void _writeReg(uint8_t reg);
    TouchPoint _last;
    uint32_t _last_read = 0;
};

extern TouchDriver touch;
