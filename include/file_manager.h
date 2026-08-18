#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>

struct FileInfo {
    String name;
    String path;
    bool isDir;
    size_t size;
};

class FileManager {
public:
    bool begin();
    std::vector<FileInfo> listDir(const char* path);
    std::vector<FileInfo> getMusicFiles();
    std::vector<FileInfo> getVideoFiles();
    std::vector<FileInfo> getBookFiles();
    bool exists(const char* path);
    File openFile(const char* path, const char* mode = FILE_READ);

private:
    bool _initialized = false;
};

extern FileManager fileManager;
