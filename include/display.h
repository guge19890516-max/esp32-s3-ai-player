#pragma once
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include "config.h"

class LGFX : public lgfx::LGFX_Device {
public:
    LGFX() {
        auto *bus = new lgfx::Bus_SPI();
        {
            auto cfg = bus->config();
            cfg.pin_mosi = TFT_MOSI;
            cfg.pin_miso = TFT_MISO;
            cfg.pin_sclk = TFT_SCLK;
            cfg.pin_dc   = TFT_DC;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            bus->config(cfg);
        }

        auto *panel = new lgfx::Panel_ST7796();
        {
            auto cfg = panel->config();
            cfg.pin_cs   = TFT_CS;
            cfg.pin_rst  = TFT_RST;
            cfg.memory_width  = 480;
            cfg.memory_height = 320;
            cfg.panel_width   = 480;
            cfg.panel_height  = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 1;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            panel->config(cfg);
        }

        panel->setBus(bus);
        setPanel(panel);
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
