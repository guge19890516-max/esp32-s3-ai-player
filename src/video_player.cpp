#include "video_player.h"
#include "config.h"
#include "display.h"
#include <SD.h>

VideoPlayer videoPlayer;

bool VideoPlayer::begin() {
    _frame_buffer = (uint8_t*)ps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    if (!_frame_buffer) {
        Serial.println("Video: PSRAM alloc failed");
        return false;
    }
    Serial.println("Video player initialized");
    return true;
}

bool VideoPlayer::play(const char* path) {
    _file = SD.open(path, FILE_READ);
    if (!_file) {
        Serial.printf("Failed to open video: %s\n", path);
        return false;
    }
    
    _playing = true;
    _paused = false;
    _frame_count = 0;
    
    Serial.printf("Playing video: %s\n", path);
    return true;
}

void VideoPlayer::pause() {
    _paused = !_paused;
}

void VideoPlayer::stop() {
    _playing = false;
    _paused = false;
    if (_file) {
        _file.close();
    }
}

void VideoPlayer::update() {
    if (!_playing || _paused || !_file) return;
    
    // Simple MJPEG/RAW frame reader
    // Each frame: 480*320*2 bytes (RGB565)
    size_t frame_size = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    
    if (_file.available() < frame_size) {
        stop();
        return;
    }
    
    size_t read = _file.read(_frame_buffer, frame_size);
    if (read == frame_size) {
        // Push frame to display
        lcd.startWrite();
        lcd.setAddrWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        lcd.writePixels((uint16_t*)_frame_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
        lcd.endWrite();
        _frame_count++;
        
        // Frame rate control
        delay(1000 / _fps);
    }
}

uint32_t VideoPlayer::getPosition() {
    if (!_file) return 0;
    return _frame_count;
}

uint32_t VideoPlayer::getDuration() {
    if (!_file) return 0;
    size_t frame_size = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    return _file.size() / frame_size;
}
