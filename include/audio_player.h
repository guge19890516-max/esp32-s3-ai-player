#pragma once
#include <Arduino.h>
#include <SD.h>

enum AudioState { AUDIO_IDLE, AUDIO_PLAYING, AUDIO_PAUSED, AUDIO_STOPPED };

class AudioPlayer {
public:
    bool begin();
    bool play(const char* path);
    void pause();
    void resume();
    void stop();
    void setVolume(uint8_t vol);
    uint8_t getVolume() { return _volume; }
    AudioState getState() { return _state; }
    uint32_t getPosition();
    uint32_t getDuration();
    void update();
    bool isMp3(const char* path);
    bool isWav(const char* path);

private:
    AudioState _state = AUDIO_IDLE;
    uint8_t _volume = 80;
    SDFile SDFile;
    uint32_t _file_size = 0;
    uint8_t _buffer[2048];
};

extern AudioPlayer audioPlayer;
