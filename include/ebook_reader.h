#pragma once
#include <Arduino.h>
#include <vector>

enum BookFormat { BOOK_TXT, BOOK_EPUB, BOOK_UNKNOWN };

struct BookPage {
    String text;
    int line_count;
};

class EbookReader {
public:
    bool begin();
    bool openBook(const char* path);
    void closeBook();
    BookPage getPage(int page_num);
    int getTotalPages();
    void nextPage();
    void prevPage();
    int getCurrentPage() { return _current_page; }
    bool isOpen() { return _book_open; }
    BookFormat getFormat() { return _format; }
    String getTitle() { return _title; }
    void setPageSize(int lines) { _page_size = lines; }

private:
    bool _book_open = false;
    BookFormat _format = BOOK_UNKNOWN;
    String _title;
    int _current_page = 0;
    int _total_pages = 0;
    int _page_size = 18; // lines per page
    std::vector<String> _lines;
    
    void _loadTxt(const char* path);
    void _loadEpub(const char* path);
    void _calculatePages();
};

extern EbookReader ebookReader;
