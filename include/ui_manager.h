#pragma once
#include <Arduino.h>
#include <lvgl.h>

enum UIScreen {
    SCREEN_MENU,
    SCREEN_MUSIC,
    SCREEN_VIDEO,
    SCREEN_BOOK,
    SCREEN_AI,
    SCREEN_SETTINGS,
    SCREEN_WIFI,
    SCREEN_BROWSER
};

class UIManager {
public:
    void begin();
    void switchScreen(UIScreen screen);
    void update();
    UIScreen getCurrentScreen() { return _current; }

    // Menu
    void createMenu();
    // Music player
    void createMusicPlayer();
    void updateMusicInfo(const char* title, const char* artist);
    void updateMusicProgress(uint32_t pos, uint32_t dur);
    // Video player
    void createVideoPlayer();
    // Book reader
    void createBookReader();
    void updateBookContent(const char* text, int page, int total);
    // AI Chat
    void createAIChat();
    void addChatMessage(const char* msg, bool isUser);
    // Settings
    void createSettings();
    // WiFi settings
    void createWiFiSettings();
    // File browser
    void createFileBrowser(const char* path, int filter_type);

private:
    UIScreen _current = SCREEN_MENU;
    lv_obj_t* _scr = nullptr;
    
    // Music player widgets
    lv_obj_t* _music_title = nullptr;
    lv_obj_t* _music_artist = nullptr;
    lv_obj_t* _music_progress = nullptr;
    lv_obj_t* _music_slider = nullptr;
    lv_obj_t* _music_time = nullptr;
    
    // AI Chat widgets
    lv_obj_t* _chat_list = nullptr;
    lv_obj_t* _chat_input = nullptr;
    
    // Book reader widgets
    lv_obj_t* _book_text = nullptr;
    lv_obj_t* _book_page_label = nullptr;
    lv_obj_t* _book_title_label = nullptr;
    
    // WiFi settings widgets
    lv_obj_t* _wifi_ssid_ta = nullptr;
    lv_obj_t* _wifi_pass_ta = nullptr;
    lv_obj_t* _wifi_status_label = nullptr;
    lv_obj_t* _wifi_scan_list = nullptr;
    
    // File browser widgets
    lv_obj_t* _file_list = nullptr;
    lv_obj_t* _file_path_label = nullptr;
    String _current_path = "/";
    int _file_filter = 0; // 0=all, 1=audio, 2=video, 3=books
    
    void _createNavBar(lv_obj_t* parent, const char* title);
    void _updateWiFiStatus();

public:
    // Accessors for event callbacks
    lv_obj_t* getWifiSSID() { return _wifi_ssid_ta; }
    lv_obj_t* getWifiPass() { return _wifi_pass_ta; }
    lv_obj_t* getWifiStatus() { return _wifi_status_label; }
    lv_obj_t* getMusicTitle() { return _music_title; }
    lv_obj_t* getDispDrv() { return _scr; }
    int getFileFilter() { return _file_filter; }
};

extern UIManager ui;
