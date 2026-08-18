#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

class LGFX : public lovyan::LovyanGFX {
public:
    LGFX() : LovyanGFX(new config_t()) {}

    void init() override {
        auto cfg = _config;
        cfg.pin_mosi = TFT_MOSI;
        cfg.pin_miso = TFT_MISO;
        cfg.pin_sclk = TFT_SCLK;
        cfg.pin_cs   = TFT_CS;
        cfg.pin_dc   = TFT_DC;
        cfg.pin_rst  = TFT_RST;
        cfg.bus_shared = true;
        cfg.freq_write = 40000000;
        cfg.freq_read  = 16000000;
        cfg.panel = new lgfx::Panel_ST7796();
        _config = cfg;
        LovyanGFX::init();
        setRotation(1);
    }

    void initBL() {
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, TFT_BL_ON);
    }
};

extern LGFX lcd;

// Display manager
class DisplayManager {
public:
    void begin();
    void update();
    void setBrightness(uint8_t level);

    // LVGL helpers
    static void lvgl_flush_cb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
    static void lvgl_touch_cb(lv_indev_drv_t* indev, lv_indev_data_t* data);

    lv_disp_drv_t* getDispDrv() { return &_disp_drv; }
    lv_indev_drv_t* getTouchDrv() { return &_indev_drv; }

private:
    lv_disp_drv_t _disp_drv;
    lv_indev_drv_t _indev_drv;
    static uint32_t _last_tick;
};

extern DisplayManager display;
