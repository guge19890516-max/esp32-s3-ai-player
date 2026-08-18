#include "ai_chat.h"
#include "config.h"
#include <WiFi.h>
#include <ArduinoJson.h>

AIChat aiChat;

bool AIChat::begin() {
    _api_key = DEEPSEEK_API_KEY;
    _model = DEEPSEEK_MODEL;
    _api_url = DEEPSEEK_API_URL;
    _connected = WiFi.status() == WL_CONNECTED;
    
    if (_connected) {
        Serial.println("AI Chat ready (DeepSeek)");
    } else {
        Serial.println("AI Chat: No WiFi connection");
    }
    return true;
}

void AIChat::setApiKey(const char* key) {
    _api_key = String(key);
}

void AIChat::setModel(const char* model) {
    _model = String(model);
}

String AIChat::_buildRequestBody(const char* message) {
    JsonDocument doc;
    
    doc["model"] = _model;
    doc["max_tokens"] = 1024;
    doc["temperature"] = 0.7;
    
    JsonArray messages = doc["messages"].to<JsonArray>();
    
    // System message
    JsonObject system = messages.add<JsonObject>();
    system["role"] = "system";
    system["content"] = "你是一个友好的AI助手，运行在ESP32开发板上。请用简洁的中文回答问题。回答尽量简短，适合在小屏幕上阅读。";
    
    // History
    for (auto& msg : _history) {
        JsonObject m = messages.add<JsonObject>();
        m["role"] = msg.role;
        m["content"] = msg.content;
    }
    
    // Current message
    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user";
    user["content"] = message;
    
    String output;
    serializeJson(doc, output);
    return output;
}

String AIChat::_parseResponse(const String& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return "解析响应失败: " + String(error.c_str());
    }
    
    if (doc["choices"].is<JsonArray>() && doc["choices"].size() > 0) {
        return doc["choices"][0]["message"]["content"].as<String>();
    }
    
    return "未收到有效响应";
}

String AIChat::chat(const char* userMessage) {
    if (!WiFi.status() == WL_CONNECTED) {
        return "未连接网络，请先连接WiFi";
    }
    
    if (_api_key.length() == 0) {
        return "请先设置API Key";
    }
    
    HTTPClient http;
    http.begin(_api_url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + _api_key);
    
    String body = _buildRequestBody(userMessage);
    
    int httpCode = http.POST(body);
    
    if (httpCode != 200) {
        String error = http.getString();
        http.end();
        return "请求失败 (" + String(httpCode) + "): " + error.substring(0, 100);
    }
    
    String response = http.getString();
    http.end();
    
    String answer = _parseResponse(response);
    
    // Save to history
    AIMessage user_msg = {"user", String(userMessage)};
    AIMessage assistant_msg = {"assistant", answer};
    _history.push_back(user_msg);
    _history.push_back(assistant_msg);
    
    // Keep history manageable (max 20 messages)
    while (_history.size() > 20) {
        _history.erase(_history.begin());
    }
    
    _last_response = answer;
    return answer;
}

String AIChat::chatWithHistory(const char* userMessage) {
    return chat(userMessage);
}

void AIChat::clearHistory() {
    _history.clear();
    Serial.println("Chat history cleared");
}

String AIChat::recordAndTranscribe() {
    // Placeholder for voice recording and transcription
    // In production, integrate with:
    // 1. I2S microphone recording
    // 2. Speech-to-text API (e.g., Whisper, Deepgram)
    return "语音识别功能待集成";
}

void AIChat::speak(const char* text) {
    // Placeholder for text-to-speech
    // In production, integrate with:
    // 1. TTS API (e.g., Edge TTS, Azure TTS)
    // 2. Audio playback through I2S
    Serial.printf("TTS: %s\n", text);
}
