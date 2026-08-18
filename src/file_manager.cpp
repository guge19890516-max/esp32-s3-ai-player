#include "file_manager.h"
#include "config.h"
#include <algorithm>
#include <SPI.h>

FileManager fileManager;

static SPIClass sdSPI(HSPI);

bool FileManager::begin() {
    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI, 40000000)) {
        Serial.println("SD card init failed");
        return false;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return false;
    }
    
    Serial.printf("SD Card: %lluMB\n", SD.cardSize() / (1024 * 1024));
    _initialized = true;
    return true;
}

std::vector<FileInfo> FileManager::listDir(const char* path) {
    std::vector<FileInfo> files;
    
    if (!_initialized) return files;
    
    File root = SD.open(path);
    if (!root || !root.isDirectory()) return files;
    
    File file = root.openNextFile();
    while (file && files.size() < MAX_FILE_LIST) {
        FileInfo info;
        info.name = String(file.name());
        info.path = String(path) + "/" + info.name;
        info.isDir = file.isDirectory();
        info.size = file.size();
        files.push_back(info);
        file = root.openNextFile();
    }
    root.close();
    
    // Sort: directories first, then alphabetically
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        String la = a.name; la.toLowerCase();
        String lb = b.name; lb.toLowerCase();
        return la < lb;
    });
    
    return files;
}

std::vector<FileInfo> FileManager::getMusicFiles() {
    std::vector<FileInfo> music;
    std::vector<FileInfo> all = listDir("/");
    
    for (auto& f : all) {
        if (!f.isDir) {
            String name = f.name;
            name.toLowerCase();
            if (name.endsWith(".mp3") || name.endsWith(".wav") || 
                name.endsWith(".aac") || name.endsWith(".m4a") ||
                name.endsWith(".flac") || name.endsWith(".ogg")) {
                music.push_back(f);
            }
        }
    }
    return music;
}

std::vector<FileInfo> FileManager::getVideoFiles() {
    std::vector<FileInfo> videos;
    std::vector<FileInfo> all = listDir("/");
    
    for (auto& f : all) {
        if (!f.isDir) {
            String name = f.name;
            name.toLowerCase();
            if (name.endsWith(".avi") || name.endsWith(".mjpeg") || 
                name.endsWith(".mpg") || name.endsWith(".bmp_seq")) {
                videos.push_back(f);
            }
        }
    }
    return videos;
}

std::vector<FileInfo> FileManager::getBookFiles() {
    std::vector<FileInfo> books;
    std::vector<FileInfo> all = listDir("/");
    
    for (auto& f : all) {
        if (!f.isDir) {
            String name = f.name;
            name.toLowerCase();
            if (name.endsWith(".txt") || name.endsWith(".epub") || 
                name.endsWith(".md") || name.endsWith(".log")) {
                books.push_back(f);
            }
        }
    }
    return books;
}

bool FileManager::exists(const char* path) {
    return SD.exists(path);
}

File FileManager::openFile(const char* path, const char* mode) {
    return SD.open(path, mode);
}
