#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

class WiFiManager {
public:
    bool begin();
    bool connect(const char* ssid, const char* password);
    void disconnect();
    bool isConnected() { return WiFi.status() == WL_CONNECTED; }
    String getIP() { return WiFi.localIP().toString(); }
    String getSSID() { return WiFi.SSID(); }
    int getRSSI() { return WiFi.RSSI(); }
    void scanNetworks();
    int getScanCount() { return _scan_count; }
    String getScanResult(int index);
    
    // NVS Storage
    void saveCredentials(const char* ssid, const char* password);
    bool loadCredentials(String& ssid, String& password);
    void clearCredentials();
    bool hasStoredCredentials() { return _has_stored; }

private:
    int _scan_count = 0;
    String* _scan_results = nullptr;
    Preferences _prefs;
    bool _has_stored = false;
};

extern WiFiManager wifiManager;
