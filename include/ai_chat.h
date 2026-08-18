#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

struct AIMessage {
    String role;    // "user" or "assistant"
    String content;
};

class AIChat {
public:
    bool begin();
    void setApiKey(const char* key);
    void setModel(const char* model);
    String chat(const char* userMessage);
    String chatWithHistory(const char* userMessage);
    void clearHistory();
    bool isConnected() { return _connected; }
    String getLastResponse() { return _last_response; }
    
    // Voice
    String recordAndTranscribe();
    void speak(const char* text);

private:
    bool _connected = false;
    String _api_key;
    String _model = "deepseek-chat";
    String _api_url = "https://api.deepseek.com/v1/chat/completions";
    std::vector<AIMessage> _history;
    String _last_response;
    String _buildRequestBody(const char* message);
    String _parseResponse(const String& json);
};

extern AIChat aiChat;
