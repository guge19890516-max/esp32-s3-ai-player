#pragma once
#include <Arduino.h>

class VideoPlayer {
public:
    bool begin();
    bool play(const char* path);
    void pause();
    void resume();
    void stop();
    void update();
    bool isPlaying() { return _playing; }
    uint32_t getPosition();
    uint32_t getDuration();

private:
    bool _playing = false;
    bool _paused = false;
    File _file;
    uint32_t _frame_count = 0;
    uint32_t _fps = 15;
    uint8_t* _frame_buffer = nullptr;
};

extern VideoPlayer videoPlayer;
