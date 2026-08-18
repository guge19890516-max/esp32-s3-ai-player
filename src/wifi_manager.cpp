#include "wifi_manager.h"
#include "config.h"

WiFiManager wifiManager;

bool WiFiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    // Try to load stored credentials
    String ssid, password;
    if (loadCredentials(ssid, password) && ssid.length() > 0) {
        Serial.printf("Found stored WiFi: %s\n", ssid.c_str());
        _has_stored = true;
        // Auto-connect in background
        connect(ssid.c_str(), password.c_str());
    }
    
    Serial.println("WiFi initialized");
    return true;
}

bool WiFiManager::connect(const char* ssid, const char* password) {
    if (strlen(ssid) == 0) return false;
    
    Serial.printf("Connecting to %s...\n", ssid);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    
    Serial.println("Connection failed");
    return false;
}

void WiFiManager::disconnect() {
    WiFi.disconnect();
    Serial.println("WiFi disconnected");
}

void WiFiManager::scanNetworks() {
    if (_scan_results) {
        delete[] _scan_results;
        _scan_results = nullptr;
    }
    _scan_count = WiFi.scanNetworks();
    Serial.printf("Found %d networks\n", _scan_count);
    
    if (_scan_count > 0) {
        _scan_results = new String[_scan_count];
        for (int i = 0; i < _scan_count; i++) {
            _scan_results[i] = WiFi.SSID(i);
        }
    }
}

String WiFiManager::getScanResult(int index) {
    if (index < 0 || index >= _scan_count) return "";
    return _scan_results[index];
}

void WiFiManager::saveCredentials(const char* ssid, const char* password) {
    _prefs.begin("wifi", false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("password", password);
    _prefs.end();
    _has_stored = true;
    Serial.printf("WiFi credentials saved: %s\n", ssid);
}

bool WiFiManager::loadCredentials(String& ssid, String& password) {
    _prefs.begin("wifi", true);
    ssid = _prefs.getString("ssid", "");
    password = _prefs.getString("password", "");
    _prefs.end();
    return ssid.length() > 0;
}

void WiFiManager::clearCredentials() {
    _prefs.begin("wifi", false);
    _prefs.clear();
    _prefs.end();
    _has_stored = false;
    Serial.println("WiFi credentials cleared");
}
