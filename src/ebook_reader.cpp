#include "ebook_reader.h"
#include "config.h"
#include <SD.h>

EbookReader ebookReader;

bool EbookReader::begin() {
    Serial.println("E-book reader initialized");
    return true;
}

bool EbookReader::openBook(const char* path) {
    closeBook();
    
    String pathStr(path);
    pathStr.toLowerCase();
    
    if (pathStr.endsWith(".txt") || pathStr.endsWith(".md") || pathStr.endsWith(".log")) {
        _format = BOOK_TXT;
        _loadTxt(path);
    } else if (pathStr.endsWith(".epub")) {
        _format = BOOK_EPUB;
        _loadEpub(path);
    } else {
        _format = BOOK_UNKNOWN;
        return false;
    }
    
    if (_lines.size() == 0) {
        return false;
    }
    
    _book_open = true;
    _current_page = 0;
    _calculatePages();
    
    // Extract title from filename
    String filename(path);
    int lastSlash = filename.lastIndexOf('/');
    if (lastSlash >= 0) {
        _title = filename.substring(lastSlash + 1);
    } else {
        _title = filename;
    }
    int lastDot = _title.lastIndexOf('.');
    if (lastDot >= 0) {
        _title = _title.substring(0, lastDot);
    }
    
    Serial.printf("Book opened: %s (%d lines, %d pages)\n", _title.c_str(), _lines.size(), _total_pages);
    return true;
}

void EbookReader::closeBook() {
    _lines.clear();
    _book_open = false;
    _current_page = 0;
    _total_pages = 0;
    _format = BOOK_UNKNOWN;
}

void EbookReader::_loadTxt(const char* path) {
    File file = SD.open(path, FILE_READ);
    if (!file) {
        Serial.printf("Failed to open book: %s\n", path);
        return;
    }
    
    String line;
    while (file.available() && _lines.size() < 10000) {
        line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            _lines.push_back(line);
        }
    }
    file.close();
    Serial.printf("Loaded %d lines from TXT\n", _lines.size());
}

void EbookReader::_loadEpub(const char* path) {
    // Simplified EPUB reader - extracts text content
    // In production, use a proper EPUB library
    File file = SD.open(path, FILE_READ);
    if (!file) {
        Serial.printf("Failed to open EPUB: %s\n", path);
        return;
    }
    
    // For now, read as raw text (EPUB is ZIP, need unzip library)
    // TODO: Implement proper EPUB parsing with unzip
    Serial.println("EPUB support requires unzip library");
    Serial.println("Loading as plain text for now...");
    
    // Fallback: try to read as text
    _loadTxt(path);
    file.close();
}

void EbookReader::_calculatePages() {
    if (_lines.size() == 0) {
        _total_pages = 0;
        return;
    }
    _total_pages = (_lines.size() + _page_size - 1) / _page_size;
}

BookPage EbookReader::getPage(int page_num) {
    BookPage page;
    page.text = "";
    page.line_count = 0;
    
    if (page_num < 0 || page_num >= _total_pages) return page;
    
    int start_line = page_num * _page_size;
    int end_line = min(start_line + _page_size, (int)_lines.size());
    
    for (int i = start_line; i < end_line; i++) {
        page.text += _lines[i] + "\n";
        page.line_count++;
    }
    
    return page;
}

int EbookReader::getTotalPages() {
    return _total_pages;
}

void EbookReader::nextPage() {
    if (_current_page < _total_pages - 1) {
        _current_page++;
    }
}

void EbookReader::prevPage() {
    if (_current_page > 0) {
        _current_page--;
    }
}
