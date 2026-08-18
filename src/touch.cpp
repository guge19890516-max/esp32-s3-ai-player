#include "touch.h"
#include "config.h"

TouchDriver touch;

#define CST816_ADDR 0x15

bool TouchDriver::begin() {
    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
    
    if (TOUCH_RST >= 0) {
        pinMode(TOUCH_RST, OUTPUT);
        digitalWrite(TOUCH_RST, LOW);
        delay(10);
        digitalWrite(TOUCH_RST, HIGH);
        delay(50);
    }
    
    pinMode(TOUCH_INT, INPUT);
    
    // Check if touch controller responds
    Wire.beginTransmission(CST816_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("Touch controller not found");
        return false;
    }
    
    Serial.println("Touch initialized");
    return true;
}

uint8_t TouchDriver::_readReg(uint8_t reg) {
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)1);
    return Wire.read();
}

void TouchDriver::_writeReg(uint8_t reg) {
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(reg);
    Wire.endTransmission();
}

TouchPoint TouchDriver::read() {
    TouchPoint tp = {0, 0, false};
    
    uint8_t fingers = _readReg(0x02);
    if (fingers > 0) {
        uint8_t xh = _readReg(0x03);
        uint8_t xl = _readReg(0x04);
        uint8_t yh = _readReg(0x05);
        uint8_t yl = _readReg(0x06);
        
        tp.x = ((xh & 0x0F) << 8) | xl;
        tp.y = ((yh & 0x0F) << 8) | yl;
        tp.pressed = true;
        
        _last = tp;
    }
    
    return _last;
}

bool TouchDriver::isTouched() {
    return digitalRead(TOUCH_INT) == LOW;
}
