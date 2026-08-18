#include "display.h"
#include "config.h"

LGFX lcd;
DisplayManager display;

// LVGL buffer
static lv_disp_draw_buf_t _draw_buf;
static lv_color_t* _buf1 = nullptr;
static lv_color_t* _buf2 = nullptr;

uint32_t DisplayManager::_last_tick = 0;

void DisplayManager::begin() {
    lcd.init();
    lcd.initBL();
    lcd.fillScreen(TFT_BLACK);
    lcd.setRotation(1);

    // Init LVGL
    lv_init();

    // Allocate PSRAM buffers
    _buf1 = (lv_color_t*)ps_malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t));
    _buf2 = (lv_color_t*)ps_malloc(SCREEN_WIDTH * 40 * sizeof(lv_color_t));
    
    if (!_buf1 || !_buf2) {
        Serial.println("PSRAM alloc failed, using internal RAM");
        _buf1 = (lv_color_t*)malloc(SCREEN_WIDTH * 20 * sizeof(lv_color_t));
        _buf2 = NULL;
    }

    lv_disp_draw_buf_init(&_draw_buf, _buf1, _buf2, SCREEN_WIDTH * 40);

    // Init display driver
    lv_disp_drv_init(&_disp_drv);
    _disp_drv.hor_res = SCREEN_WIDTH;
    _disp_drv.ver_res = SCREEN_HEIGHT;
    _disp_drv.flush_cb = lvgl_flush_cb;
    _disp_drv.draw_buf = &_draw_buf;
    _disp_drv.full_refresh = false;
    lv_disp_drv_register(&_disp_drv);

    _last_tick = millis();
    Serial.println("Display initialized");
}

void DisplayManager::update() {
    uint32_t tick = millis() - _last_tick;
    _last_tick = millis();
    lv_timer_handler();
}

void DisplayManager::setBrightness(uint8_t level) {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, level);
}

void DisplayManager::lvgl_flush_cb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((uint16_t*)color_p, w * h);
    lcd.endWrite();
    lv_disp_flush_ready(disp);
}

void DisplayManager::lvgl_touch_cb(lv_indev_drv_t* indev, lv_indev_data_t* data) {
    // Touch reading is handled separately
    data->state = LV_INDEV_STATE_REL;
}
