#include "ui_manager.h"
#include "config.h"
#include <WiFi.h>
#include "audio_player.h"
#include "ebook_reader.h"
#include "ai_chat.h"
#include "file_manager.h"
#include "wifi_manager.h"

UIManager ui;

// Global screen objects
static lv_obj_t* g_scr_menu = NULL;
static lv_obj_t* g_scr_music = NULL;
static lv_obj_t* g_scr_video = NULL;
static lv_obj_t* g_scr_book = NULL;
static lv_obj_t* g_scr_ai = NULL;
static lv_obj_t* g_scr_settings = NULL;
static lv_obj_t* g_scr_wifi = NULL;
static lv_obj_t* g_scr_browser = NULL;

// Music player state
static int current_music_index = 0;
static std::vector<FileInfo> music_files;

// ========== Event Callbacks ==========
static void menu_event_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    UIScreen screen = (UIScreen)id;
    ui.switchScreen(screen);
}

static void music_play_cb(lv_event_t* e) {
    if (music_files.size() == 0) return;
    audioPlayer.play(music_files[current_music_index].path.c_str());
    lv_label_set_text(ui.getDispDrv() ? ui.getMusicTitle() : NULL, 
                      music_files[current_music_index].name.c_str());
}

static void music_pause_cb(lv_event_t* e) {
    audioPlayer.pause();
}

static void music_stop_cb(lv_event_t* e) {
    audioPlayer.stop();
}

static void music_next_cb(lv_event_t* e) {
    if (music_files.size() == 0) return;
    current_music_index = (current_music_index + 1) % music_files.size();
    audioPlayer.play(music_files[current_music_index].path.c_str());
}

static void music_prev_cb(lv_event_t* e) {
    if (music_files.size() == 0) return;
    current_music_index = (current_music_index - 1 + music_files.size()) % music_files.size();
    audioPlayer.play(music_files[current_music_index].path.c_str());
}

static void ai_send_cb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_user_data(e);
    const char* text = lv_textarea_get_text(ta);
    if (strlen(text) == 0) return;
    
    // Add user message
    ui.addChatMessage(text, true);
    
    // Get AI response
    String response = aiChat.chat(text);
    ui.addChatMessage(response.c_str(), false);
    
    // Clear input
    lv_textarea_set_text(ta, "");
}

static void book_prev_cb(lv_event_t* e) {
    ebookReader.prevPage();
    BookPage page = ebookReader.getPage(ebookReader.getCurrentPage());
    ui.updateBookContent(page.text.c_str(), ebookReader.getCurrentPage(), ebookReader.getTotalPages());
}

static void book_next_cb(lv_event_t* e) {
    ebookReader.nextPage();
    BookPage page = ebookReader.getPage(ebookReader.getCurrentPage());
    ui.updateBookContent(page.text.c_str(), ebookReader.getCurrentPage(), ebookReader.getTotalPages());
}

// WiFi Settings Callbacks
static void wifi_scan_cb(lv_event_t* e) {
    wifiManager.scanNetworks();
    ui.updateWiFiStatus();
}

static void wifi_connect_cb(lv_event_t* e) {
    const char* ssid = lv_textarea_get_text(ui.getWifiSSID());
    const char* pass = lv_textarea_get_text(ui.getWifiPass());
    
    if (strlen(ssid) == 0) return;
    
    lv_label_set_text(ui.getWifiStatus(), "Connecting...");
    
    if (wifiManager.connect(ssid, pass)) {
        wifiManager.saveCredentials(ssid, pass);
        lv_label_set_text(ui.getWifiStatus(), "Connected!");
    } else {
        lv_label_set_text(ui.getWifiStatus(), "Failed!");
    }
}

static void wifi_network_click_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    const char* ssid = lv_label_get_text(lv_obj_get_child(btn, 0));
    lv_textarea_set_text(ui.getWifiSSID(), ssid);
}

// File Browser Callbacks
static void file_click_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    const char* path = (const char*)lv_event_get_user_data(e);
    
    // Check if it's a directory
    File f = SD.open(path);
    bool is_dir = f && f.isDirectory();
    f.close();
    
    if (is_dir) {
        ui.createFileBrowser(path, ui.getFileFilter());
    } else {
        // Open file based on type
        String pathStr(path);
        pathStr.toLowerCase();
        
        if (pathStr.endsWith(".mp3") || pathStr.endsWith(".wav")) {
            audioPlayer.play(path);
        } else if (pathStr.endsWith(".txt") || pathStr.endsWith(".epub")) {
            ebookReader.openBook(path);
        }
    }
}

// ========== UI Creation ==========
void UIManager::begin() {
    createMenu();
}

void UIManager::_createNavBar(lv_obj_t* parent, const char* title) {
    lv_obj_t* nav = lv_obj_create(parent);
    lv_obj_set_size(nav, SCREEN_WIDTH, 50);
    lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x0F3460), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(nav);
    lv_obj_set_size(back_btn, 40, 35);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x533483), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, menu_event_cb, LV_EVENT_CLICKED, (void*)SCREEN_MENU);
    
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
    
    // Title
    lv_obj_t* title_label = lv_label_create(nav);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);
}

void UIManager::createMenu() {
    if (g_scr_menu) lv_obj_del(g_scr_menu);
    
    g_scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_menu, COLOR_BG, 0);
    
    // Title
    lv_obj_t* title = lv_label_create(g_scr_menu);
    lv_label_set_text(title, "ESP32 Player");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    // Subtitle
    lv_obj_t* subtitle = lv_label_create(g_scr_menu);
    lv_label_set_text(subtitle, "AI Music Video Book");
    lv_obj_set_style_text_color(subtitle, COLOR_TEXT_DIM, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 55);
    
    // Menu buttons - 2x2 grid
    const char* labels[] = {"Music", "Video", "E-Book", "AI Chat"};
    const lv_color_t colors[] = {
        lv_color_hex(0x00B894),
        lv_color_hex(0x0984E3),
        lv_color_hex(0xFDAA00),
        lv_color_hex(0xE74C3C)
    };
    int screens[] = {SCREEN_MUSIC, SCREEN_VIDEO, SCREEN_BOOK, SCREEN_AI};
    
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        
        lv_obj_t* btn = lv_btn_create(g_scr_menu);
        lv_obj_set_size(btn, 180, 120);
        lv_obj_align(btn, LV_ALIGN_CENTER, (col - 0.5) * 200, (row - 0.5) * 150);
        lv_obj_set_style_bg_color(btn, colors[i], 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_shadow_width(btn, 10, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
        lv_obj_add_event_cb(btn, menu_event_cb, LV_EVENT_CLICKED, (void*)screens[i]);
        
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
        lv_obj_center(label);
    }
    
    // Bottom row - Settings and WiFi
    lv_obj_t* settings_btn = lv_btn_create(g_scr_menu);
    lv_obj_set_size(settings_btn, 85, 50);
    lv_obj_align(settings_btn, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(settings_btn, COLOR_HIGHLIGHT, 0);
    lv_obj_set_style_radius(settings_btn, 10, 0);
    lv_obj_add_event_cb(settings_btn, menu_event_cb, LV_EVENT_CLICKED, (void*)SCREEN_SETTINGS);
    
    lv_obj_t* settings_label = lv_label_create(settings_btn);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(settings_label, COLOR_TEXT, 0);
    lv_obj_center(settings_label);
    
    lv_obj_t* wifi_btn = lv_btn_create(g_scr_menu);
    lv_obj_set_size(wifi_btn, 85, 50);
    lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(wifi_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(wifi_btn, 10, 0);
    lv_obj_add_event_cb(wifi_btn, menu_event_cb, LV_EVENT_CLICKED, (void*)SCREEN_WIFI);
    
    lv_obj_t* wifi_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_label, COLOR_TEXT, 0);
    lv_obj_center(wifi_label);
    
    lv_scr_load(g_scr_menu);
}

void UIManager::createMusicPlayer() {
    if (g_scr_music) lv_obj_del(g_scr_music);
    
    g_scr_music = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_music, COLOR_BG, 0);
    
    _createNavBar(g_scr_music, "Music Player");
    
    // Album art placeholder
    lv_obj_t* art = lv_obj_create(g_scr_music);
    lv_obj_set_size(art, 200, 200);
    lv_obj_align(art, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(art, COLOR_CARD, 0);
    lv_obj_set_style_radius(art, 20, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* art_icon = lv_label_create(art);
    lv_label_set_text(art_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(art_icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(art_icon, COLOR_TEXT_DIM, 0);
    lv_obj_center(art_icon);
    
    // Song title
    _music_title = lv_label_create(g_scr_music);
    lv_label_set_text(_music_title, "No Song");
    lv_obj_set_style_text_color(_music_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(_music_title, &lv_font_montserrat_20, 0);
    lv_obj_align(_music_title, LV_ALIGN_CENTER, 0, 80);
    lv_label_set_long_mode(_music_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_music_title, 300);
    
    // Progress slider
    _music_slider = lv_slider_create(g_scr_music);
    lv_obj_set_width(_music_slider, 300);
    lv_obj_align(_music_slider, LV_ALIGN_CENTER, 0, 110);
    lv_slider_set_range(_music_slider, 0, 100);
    lv_obj_set_style_bg_color(_music_slider, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_music_slider, COLOR_HIGHLIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_music_slider, COLOR_TEXT, LV_PART_KNOB);
    
    // Control buttons
    lv_obj_t* ctrl_panel = lv_obj_create(g_scr_music);
    lv_obj_set_size(ctrl_panel, 300, 60);
    lv_obj_align(ctrl_panel, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_style_bg_opa(ctrl_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_panel, 0, 0);
    lv_obj_clear_flag(ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);
    
    const char* symbols[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE, 
                             LV_SYMBOL_STOP, LV_SYMBOL_NEXT};
    void (*callbacks[])(lv_event_t*) = {music_prev_cb, music_play_cb, music_pause_cb,
                                        music_stop_cb, music_next_cb};
    
    for (int i = 0; i < 5; i++) {
        lv_obj_t* btn = lv_btn_create(ctrl_panel);
        lv_obj_set_size(btn, 50, 50);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, i * 60, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
        lv_obj_set_style_radius(btn, 25, 0);
        lv_obj_add_event_cb(btn, callbacks[i], LV_EVENT_CLICKED, NULL);
        
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, symbols[i]);
        lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
        lv_obj_center(label);
    }
    
    lv_scr_load(g_scr_music);
}

void UIManager::createVideoPlayer() {
    if (g_scr_video) lv_obj_del(g_scr_video);
    
    g_scr_video = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_video, COLOR_BG, 0);
    
    _createNavBar(g_scr_video, "Video Player");
    
    // Video display area
    lv_obj_t* video_area = lv_obj_create(g_scr_video);
    lv_obj_set_size(video_area, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 80);
    lv_obj_align(video_area, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(video_area, COLOR_CARD, 0);
    lv_obj_set_style_radius(video_area, 12, 0);
    lv_obj_clear_flag(video_area, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* placeholder = lv_label_create(video_area);
    lv_label_set_text(placeholder, "Video\nPlace .avi files on SD card");
    lv_obj_set_style_text_color(placeholder, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_18, 0);
    lv_obj_center(placeholder);
    
    lv_scr_load(g_scr_video);
}

void UIManager::createBookReader() {
    if (g_scr_book) lv_obj_del(g_scr_book);
    
    g_scr_book = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_book, lv_color_hex(0xF5F0E6), 0);
    
    _createNavBar(g_scr_book, "E-Book Reader");
    lv_obj_set_style_bg_color(lv_obj_get_child(g_scr_book, 0), lv_color_hex(0xD4C5A9), 0);
    
    // Book title
    _book_title_label = lv_label_create(g_scr_book);
    lv_label_set_text(_book_title_label, "No Book");
    lv_obj_set_style_text_color(_book_title_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(_book_title_label, &lv_font_montserrat_18, 0);
    lv_obj_align(_book_title_label, LV_ALIGN_TOP_MID, 0, 60);
    
    // Book content
    _book_text = lv_label_create(g_scr_book);
    lv_label_set_text(_book_text, "Select a book to read");
    lv_obj_set_style_text_color(_book_text, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(_book_text, &lv_font_montserrat_16, 0);
    lv_obj_align(_book_text, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(_book_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_book_text, SCREEN_WIDTH - 40);
    lv_obj_set_style_text_line_space(_book_text, 8, 0);
    
    // Page controls
    lv_obj_t* prev_btn = lv_btn_create(g_scr_book);
    lv_obj_set_size(prev_btn, 100, 40);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x0984E3), 0);
    lv_obj_add_event_cb(prev_btn, book_prev_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* prev_label = lv_label_create(prev_btn);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV " Prev");
    lv_obj_set_style_text_color(prev_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(prev_label);
    
    _book_page_label = lv_label_create(g_scr_book);
    lv_label_set_text(_book_page_label, "Page 0/0");
    lv_obj_set_style_text_color(_book_page_label, lv_color_hex(0x666666), 0);
    lv_obj_align(_book_page_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    lv_obj_t* next_btn = lv_btn_create(g_scr_book);
    lv_obj_set_size(next_btn, 100, 40);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x0984E3), 0);
    lv_obj_add_event_cb(next_btn, book_next_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, "Next " LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(next_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(next_label);
    
    lv_scr_load(g_scr_book);
}

void UIManager::createAIChat() {
    if (g_scr_ai) lv_obj_del(g_scr_ai);
    
    g_scr_ai = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_ai, COLOR_BG, 0);
    
    _createNavBar(g_scr_ai, "AI Chat");
    
    // Chat list
    _chat_list = lv_obj_create(g_scr_ai);
    lv_obj_set_size(_chat_list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 130);
    lv_obj_align(_chat_list, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(_chat_list, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(_chat_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_list, 0, 0);
    lv_obj_set_style_radius(_chat_list, 12, 0);
    lv_obj_set_flex_flow(_chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(_chat_list, 10, 0);
    
    // Welcome message
    lv_obj_t* welcome = lv_label_create(_chat_list);
    lv_label_set_text(welcome, "Hello! I'm your AI assistant.\nAsk me anything!");
    lv_obj_set_style_text_color(welcome, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(welcome, &lv_font_montserrat_16, 0);
    lv_obj_set_width(welcome, SCREEN_WIDTH - 60);
    lv_label_set_long_mode(welcome, LV_LABEL_LONG_WRAP);
    
    // Input area
    lv_obj_t* input_panel = lv_obj_create(g_scr_ai);
    lv_obj_set_size(input_panel, SCREEN_WIDTH - 20, 50);
    lv_obj_align(input_panel, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(input_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_panel, 0, 0);
    lv_obj_clear_flag(input_panel, LV_OBJ_FLAG_SCROLLABLE);
    
    _chat_input = lv_textarea_create(input_panel);
    lv_obj_set_size(_chat_input, SCREEN_WIDTH - 120, 40);
    lv_obj_align(_chat_input, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_placeholder_text(_chat_input, "Type your message...");
    lv_textarea_set_one_line(_chat_input, true);
    lv_obj_set_style_bg_color(_chat_input, COLOR_CARD, 0);
    lv_obj_set_style_text_color(_chat_input, COLOR_TEXT, 0);
    lv_obj_set_style_border_color(_chat_input, COLOR_ACCENT, 0);
    
    lv_obj_t* send_btn = lv_btn_create(input_panel);
    lv_obj_set_size(send_btn, 50, 40);
    lv_obj_align(send_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(send_btn, COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(send_btn, 8, 0);
    lv_obj_add_event_cb(send_btn, ai_send_cb, LV_EVENT_CLICKED, _chat_input);
    
    lv_obj_t* send_label = lv_label_create(send_btn);
    lv_label_set_text(send_label, LV_SYMBOL_SEND);
    lv_obj_set_style_text_color(send_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(send_label);
    
    lv_scr_load(g_scr_ai);
}

void UIManager::createSettings() {
    if (g_scr_settings) lv_obj_del(g_scr_settings);
    
    g_scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_settings, COLOR_BG, 0);
    
    _createNavBar(g_scr_settings, "Settings");
    
    lv_obj_t* info = lv_label_create(g_scr_settings);
    lv_label_set_text(info, "ESP32-S3 AI Player\nVersion 1.0\n\nWiFi: Not Connected\nSD Card: Ready");
    lv_obj_set_style_text_color(info, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
    
    lv_scr_load(g_scr_settings);
}

void UIManager::createWiFiSettings() {
    if (g_scr_wifi) lv_obj_del(g_scr_wifi);
    
    g_scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_wifi, COLOR_BG, 0);
    
    _createNavBar(g_scr_wifi, "WiFi Settings");
    
    // Status label
    _wifi_status_label = lv_label_create(g_scr_wifi);
    _updateWiFiStatus();
    lv_obj_set_style_text_color(_wifi_status_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(_wifi_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(_wifi_status_label, LV_ALIGN_TOP_MID, 0, 60);
    
    // SSID input
    lv_obj_t* ssid_label = lv_label_create(g_scr_wifi);
    lv_label_set_text(ssid_label, "SSID:");
    lv_obj_set_style_text_color(ssid_label, COLOR_TEXT_DIM, 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 20, 95);
    
    _wifi_ssid_ta = lv_textarea_create(g_scr_wifi);
    lv_obj_set_size(_wifi_ssid_ta, 200, 35);
    lv_obj_align(_wifi_ssid_ta, LV_ALIGN_TOP_LEFT, 70, 90);
    lv_textarea_set_placeholder_text(_wifi_ssid_ta, "Enter SSID");
    lv_textarea_set_one_line(_wifi_ssid_ta, true);
    lv_obj_set_style_bg_color(_wifi_ssid_ta, COLOR_CARD, 0);
    lv_obj_set_style_text_color(_wifi_ssid_ta, COLOR_TEXT, 0);
    
    // Password input
    lv_obj_t* pass_label = lv_label_create(g_scr_wifi);
    lv_label_set_text(pass_label, "Pass:");
    lv_obj_set_style_text_color(pass_label, COLOR_TEXT_DIM, 0);
    lv_obj_align(pass_label, LV_ALIGN_TOP_LEFT, 20, 140);
    
    _wifi_pass_ta = lv_textarea_create(g_scr_wifi);
    lv_obj_set_size(_wifi_pass_ta, 200, 35);
    lv_obj_align(_wifi_pass_ta, LV_ALIGN_TOP_LEFT, 70, 135);
    lv_textarea_set_placeholder_text(_wifi_pass_ta, "Enter password");
    lv_textarea_set_one_line(_wifi_pass_ta, true);
    lv_textarea_set_password_mode(_wifi_pass_ta, true);
    lv_obj_set_style_bg_color(_wifi_pass_ta, COLOR_CARD, 0);
    lv_obj_set_style_text_color(_wifi_pass_ta, COLOR_TEXT, 0);
    
    // Connect button
    lv_obj_t* connect_btn = lv_btn_create(g_scr_wifi);
    lv_obj_set_size(connect_btn, 100, 40);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_LEFT, 20, 185);
    lv_obj_set_style_bg_color(connect_btn, COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(connect_btn, 8, 0);
    lv_obj_add_event_cb(connect_btn, wifi_connect_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(connect_label);
    
    // Scan button
    lv_obj_t* scan_btn = lv_btn_create(g_scr_wifi);
    lv_obj_set_size(scan_btn, 100, 40);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 130, 185);
    lv_obj_set_style_bg_color(scan_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(scan_btn, 8, 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "Scan");
    lv_obj_set_style_text_color(scan_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(scan_label);
    
    // Scan results list
    _wifi_scan_list = lv_obj_create(g_scr_wifi);
    lv_obj_set_size(_wifi_scan_list, SCREEN_WIDTH - 40, 80);
    lv_obj_align(_wifi_scan_list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(_wifi_scan_list, COLOR_CARD, 0);
    lv_obj_set_style_border_width(_wifi_scan_list, 0, 0);
    lv_obj_set_style_radius(_wifi_scan_list, 8, 0);
    lv_obj_set_flex_flow(_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    
    lv_scr_load(g_scr_wifi);
}

void UIManager::createFileBrowser(const char* path, int filter_type) {
    if (g_scr_browser) lv_obj_del(g_scr_browser);
    
    _current_path = path;
    _file_filter = filter_type;
    
    g_scr_browser = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr_browser, COLOR_BG, 0);
    
    _createNavBar(g_scr_browser, "Files");
    
    // Current path
    _file_path_label = lv_label_create(g_scr_browser);
    lv_label_set_text(_file_path_label, path);
    lv_obj_set_style_text_color(_file_path_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(_file_path_label, &lv_font_montserrat_14, 0);
    lv_obj_align(_file_path_label, LV_ALIGN_TOP_MID, 0, 55);
    
    // File list
    _file_list = lv_obj_create(g_scr_browser);
    lv_obj_set_size(_file_list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 100);
    lv_obj_align(_file_list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(_file_list, COLOR_CARD, 0);
    lv_obj_set_style_border_width(_file_list, 0, 0);
    lv_obj_set_style_radius(_file_list, 12, 0);
    lv_obj_set_flex_flow(_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_file_list, 5, 0);
    
    // List files
    File root = SD.open(path);
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file && lv_obj_get_child_cnt(_file_list) < 20) {
            String name = file.name();
            bool is_dir = file.isDirectory();
            
            // Apply filter
            bool show = true;
            if (_file_filter > 0 && !is_dir) {
                String lower = name;
                lower.toLowerCase();
                if (_file_filter == 1 && !lower.endsWith(".mp3") && !lower.endsWith(".wav")) show = false;
                if (_file_filter == 2 && !lower.endsWith(".avi") && !lower.endsWith(".mp4")) show = false;
                if (_file_filter == 3 && !lower.endsWith(".txt") && !lower.endsWith(".epub")) show = false;
            }
            
            if (show) {
                lv_obj_t* btn = lv_btn_create(_file_list);
                lv_obj_set_size(btn, SCREEN_WIDTH - 40, 35);
                lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
                lv_obj_set_style_radius(btn, 6, 0);
                lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, (void*)strdup(file.path()));
                
                lv_obj_t* icon = lv_label_create(btn);
                lv_label_set_text(icon, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
                lv_obj_set_style_text_color(icon, COLOR_TEXT_DIM, 0);
                lv_obj_align(icon, LV_ALIGN_LEFT_MID, 5, 0);
                
                lv_obj_t* label = lv_label_create(btn);
                lv_label_set_text(label, name.c_str());
                lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
                lv_obj_align(label, LV_ALIGN_LEFT_MID, 30, 0);
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
                lv_obj_set_width(label, SCREEN_WIDTH - 90);
            }
            
            file = root.openNextFile();
        }
    }
    
    lv_scr_load(g_scr_browser);
}

// ========== Public Methods ==========
void UIManager::switchScreen(UIScreen screen) {
    _current = screen;
    switch (screen) {
        case SCREEN_MENU: createMenu(); break;
        case SCREEN_MUSIC:
            music_files = fileManager.getMusicFiles();
            createMusicPlayer();
            break;
        case SCREEN_VIDEO: createVideoPlayer(); break;
        case SCREEN_BOOK: createBookReader(); break;
        case SCREEN_AI: createAIChat(); break;
        case SCREEN_SETTINGS: createSettings(); break;
        case SCREEN_WIFI: createWiFiSettings(); break;
        case SCREEN_BROWSER: createFileBrowser("/", 0); break;
        default: createMenu(); break;
    }
}

void UIManager::update() {
    // Handle periodic updates
}

void UIManager::updateMusicInfo(const char* title, const char* artist) {
    if (_music_title) lv_label_set_text(_music_title, title);
}

void UIManager::updateMusicProgress(uint32_t pos, uint32_t dur) {
    if (_music_slider && dur > 0) {
        lv_slider_set_value(_music_slider, (pos * 100) / dur, LV_ANIM_ON);
    }
}

void UIManager::updateBookContent(const char* text, int page, int total) {
    if (_book_text) lv_label_set_text(_book_text, text);
    if (_book_page_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d/%d", page + 1, total);
        lv_label_set_text(_book_page_label, buf);
    }
}

void UIManager::addChatMessage(const char* msg, bool isUser) {
    if (!_chat_list) return;
    
    lv_obj_t* bubble = lv_obj_create(_chat_list);
    lv_obj_set_width(bubble, SCREEN_WIDTH - 60);
    lv_obj_set_style_bg_color(bubble, isUser ? COLOR_ACCENT : COLOR_CARD, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, SCREEN_WIDTH - 80);
    
    // Auto scroll to bottom
    lv_obj_scroll_to_y(_chat_list, LV_COORD_MAX, LV_ANIM_ON);
}

void UIManager::_updateWiFiStatus() {
    if (!_wifi_status_label) return;
    
    if (WiFi.status() == WL_CONNECTED) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Connected to: %s\nIP: %s", 
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        lv_label_set_text(_wifi_status_label, buf);
    } else {
        lv_label_set_text(_wifi_status_label, "Not connected");
    }
}
