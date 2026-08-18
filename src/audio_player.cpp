#include "audio_player.h"
#include "config.h"
#include <SD.h>
#include <driver/i2s.h>

AudioPlayer audioPlayer;

bool AudioPlayer::begin() {
    // Configure I2S for speaker output
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN,
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("I2S install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("I2S pin config failed: %d\n", err);
        return false;
    }

    i2s_set_clk(I2S_PORT, AUDIO_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    
    Serial.println("Audio player initialized");
    return true;
}

bool AudioPlayer::play(const char* path) {
    if (_state == AUDIO_PLAYING) {
        stop();
    }

    _file = SD.open(path, FILE_READ);
    if (!_file) {
        Serial.printf("Failed to open: %s\n", path);
        return false;
    }

    _file_size = _file.size();
    _state = AUDIO_PLAYING;
    Serial.printf("Playing: %s (%d bytes)\n", path, _file_size);
    return true;
}

void AudioPlayer::pause() {
    if (_state == AUDIO_PLAYING) {
        _state = AUDIO_PAUSED;
    }
}

void AudioPlayer::resume() {
    if (_state == AUDIO_PAUSED) {
        _state = AUDIO_PLAYING;
    }
}

void AudioPlayer::stop() {
    _state = AUDIO_STOPPED;
    if (_file) {
        _file.close();
    }
    // Silence I2S
    uint8_t silence[1024] = {0};
    size_t written;
    i2s_write(I2S_PORT, silence, sizeof(silence), &written, portMAX_DELAY);
    _state = AUDIO_IDLE;
}

void AudioPlayer::setVolume(uint8_t vol) {
    _volume = vol;
    // Simple volume control via I2S clock adjustment
    // In production, use DAC or amplifier volume control
}

uint32_t AudioPlayer::getPosition() {
    if (!_file) return 0;
    return _file.position();
}

uint32_t AudioPlayer::getDuration() {
    return _file_size;
}

void AudioPlayer::update() {
    if (_state != AUDIO_PLAYING || !_file) return;

    size_t bytes_read = _file.read(_buffer, sizeof(_buffer));
    if (bytes_read == 0) {
        stop();
        return;
    }

    size_t written = 0;
    i2s_write(I2S_PORT, _buffer, bytes_read, &written, portMAX_DELAY);
}

bool AudioPlayer::isMp3(const char* path) {
    String s(path);
    s.toLowerCase();
    return s.endsWith(".mp3");
}

bool AudioPlayer::isWav(const char* path) {
    String s(path);
    s.toLowerCase();
    return s.endsWith(".wav");
}
