#pragma once

#include <Arduino.h>
#include <Preferences.h>

// ========== Display ==========
#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT 320

// ST7796 SPI Pins
#define TFT_MOSI  11
#define TFT_MISO  13
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC    14
#define TFT_RST   9
#define TFT_BL    27
#define TFT_BL_ON HIGH

// ========== Touch (CST816S) ==========
#define TOUCH_SDA   6
#define TOUCH_SCL   5
#define TOUCH_INT   4
#define TOUCH_RST  -1
#define TOUCH_I2C_ADDR 0x15

// ========== I2S Audio ==========
#define I2S_BCK   17
#define I2S_WS    47
#define I2S_DOUT  15   // Speaker
#define I2S_DIN   16   // Microphone
#define I2S_PORT  I2S_NUM_0

// ========== SD Card ==========
#define SD_CS     38
#define SD_MOSI   11
#define SD_MISO   13
#define SD_SCLK   12

// ========== Amplifier ==========
#define AMP_ENABLE 38

// ========== WiFi Default Config ==========
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#define DEEPSEEK_API_KEY ""
#define DEEPSEEK_API_URL "https://api.deepseek.com/v1/chat/completions"
#define DEEPSEEK_MODEL   "deepseek-chat"

// ========== File System ==========
#define MAX_FILENAME_LEN  128
#define MAX_PATH_LEN      256
#define MAX_FILE_LIST     100

// ========== Audio Config ==========
#define AUDIO_BUFFER_SIZE  2048
#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_VOLUME_DEFAULT 80

// ========== UI Colors ==========
#define COLOR_BG        lv_color_hex(0x1A1A2E)
#define COLOR_CARD      lv_color_hex(0x16213E)
#define COLOR_ACCENT    lv_color_hex(0x0F3460)
#define COLOR_HIGHLIGHT lv_color_hex(0x533483)
#define COLOR_TEXT      lv_color_hex(0xEAEAEA)
#define COLOR_TEXT_DIM  lv_color_hex(0x8892A0)
#define COLOR_SUCCESS   lv_color_hex(0x00B894)
#define COLOR_WARNING   lv_color_hex(0xFDAA00)
#define COLOR_ERROR     lv_color_hex(0xE74C3C)

// ========== Menu Icons (Unicode) ==========
#define ICON_MUSIC    "\xF0\x9F\x8E\xB5"
#define ICON_VIDEO    "\xF0\x9F\x8E\xAC"
#define ICON_BOOK     "\xF0\x9F\x93\x96"
#define ICON_AI       "\xF0\x9F\xA4\x96"
#define ICON_WIFI     "\xF0\x9F\x93\xB1"
#define ICON_SETTINGS "\xE2\x9A\x99\xEF\xB8\x8F"
#define ICON_BACK     "\xE2\xAC\x85\xEF\xB8\x8F"
#define ICON_PLAY     "\xE2\x96\xB6\xEF\xB8\x8F"
#define ICON_PAUSE    "\xE2\x8F\xB8\xEF\xB8\x8F"
#define ICON_STOP     "\xE2\x96\xA0\xEF\xB8\x8F"
#define ICON_NEXT     "\xE2\x8F\xB9\xEF\xB8\x8F"
#define ICON_PREV     "\xE2\x8F\xAA\xEF\xB8\x8F"
