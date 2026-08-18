#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "audio_player.h"
#include "video_player.h"
#include "file_manager.h"
#include "ai_chat.h"
#include "ebook_reader.h"
#include "ui_manager.h"
#include "wifi_manager.h"

// Task handles
TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t uiTaskHandle = NULL;

// ========== Audio Task ==========
void audioTask(void* parameter) {
    while (true) {
        audioPlayer.update();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ========== UI Task ==========
void uiTask(void* parameter) {
    while (true) {
        display.update();
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

// ========== Touch Input ==========
void readTouch(lv_indev_drv_t* indev, lv_indev_data_t* data) {
    TouchPoint tp = touch.read();
    if (tp.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = tp.x;
        data->point.y = tp.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ========== Setup ==========
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("=== ESP32-S3 AI Player ===");
    Serial.println("Initializing...");
    
    // Initialize display
    display.begin();
    Serial.println("Display ready");
    
    // Initialize touch
    if (!touch.begin()) {
        Serial.println("Touch init failed!");
    }
    
    // Register touch input with LVGL
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = readTouch;
    lv_indev_drv_register(&indev_drv);
    
    // Initialize SD card
    if (!fileManager.begin()) {
        Serial.println("SD card init failed!");
        // Show error on screen
        lv_obj_t* err_label = lv_label_create(lv_scr_act());
        lv_label_set_text(err_label, "SD Card Error!\nInsert card and restart");
        lv_obj_set_style_text_color(err_label, COLOR_ERROR, 0);
        lv_obj_center(err_label);
        while(1) { delay(1000); }
    }
    
    // Initialize audio
    if (!audioPlayer.begin()) {
        Serial.println("Audio init failed!");
    }
    
    // Initialize video player
    videoPlayer.begin();
    
    // Initialize e-book reader
    ebookReader.begin();
    
    // Initialize WiFi
    wifiManager.begin();
    
    // Initialize AI Chat
    aiChat.begin();
    
    // Initialize UI
    ui.begin();
    
    // Start audio task on core 1
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        4096,
        NULL,
        5,
        &audioTaskHandle,
        1
    );
    
    Serial.println("Initialization complete!");
    Serial.println("Ready to play.");
    
    // Show welcome screen
    ui.switchScreen(SCREEN_MENU);
}

// ========== Main Loop ==========
void loop() {
    // Update display
    display.update();
    
    // Update audio player
    audioPlayer.update();
    
    // Check WiFi status
    static uint32_t last_wifi_check = 0;
    if (millis() - last_wifi_check > 10000) {
        last_wifi_check = millis();
        if (WiFi.status() == WL_CONNECTED) {
            aiChat.begin(); // Refresh AI connection
        }
    }
    
    delay(10);
}
