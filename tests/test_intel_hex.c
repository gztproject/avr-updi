#include "hex/intel_hex.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int count;
    uint32_t address[4];
    uint8_t data[4][INTEL_HEX_PAGE_SIZE];
} PageCapture;

typedef struct {
    int count;
    char lines[8][80];
} LineCapture;

static int failures = 0;

static void check(int condition, const char* message, int line) {
    if(condition) return;
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, line, message);
    failures++;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

static bool capture_page(uint32_t address, const uint8_t data[INTEL_HEX_PAGE_SIZE], void* context) {
    PageCapture* pages = context;
    if(pages->count >= 4) return false;
    pages->address[pages->count] = address;
    memcpy(pages->data[pages->count], data, INTEL_HEX_PAGE_SIZE);
    pages->count++;
    return true;
}

static bool feed_all(const char** lines, PageCapture* pages, char* error, size_t error_size) {
    IntelHexParser* parser = intel_hex_parser_alloc(128u * 1024u, capture_page, pages);
    bool ok = parser != NULL;
    for(size_t i = 0; ok && lines[i] != NULL; i++) {
        ok = intel_hex_parser_feed(parser, lines[i], error, error_size);
    }
    if(ok) ok = intel_hex_parser_finish(parser, error, error_size);
    intel_hex_parser_free(parser);
    return ok;
}

static bool page_is_ff_except(const uint8_t* page, const uint16_t* indexes, const uint8_t* values, size_t n) {
    uint8_t expect[INTEL_HEX_PAGE_SIZE];
    memset(expect, 0xFF, sizeof(expect));
    for(size_t i = 0; i < n; i++) expect[indexes[i]] = values[i];
    return memcmp(page, expect, INTEL_HEX_PAGE_SIZE) == 0;
}

static bool capture_line(const char* line, void* context) {
    LineCapture* lines = context;
    if(lines->count >= 8) return false;
    snprintf(lines->lines[lines->count], sizeof(lines->lines[0]), "%s", line);
    lines->count++;
    return true;
}

static void test_page_crossing(void) {
    PageCapture pages;
    memset(&pages, 0, sizeof(pages));
    char error[96] = {0};
    const char* lines[] = {":0401FE001122334453", ":00000001FF", NULL};
    CHECK(feed_all(lines, &pages, error, sizeof(error)));
    CHECK(pages.count == 2);
    CHECK(pages.address[0] == 0);
    CHECK(pages.address[1] == 512);
    const uint16_t i0[] = {510, 511};
    const uint8_t v0[] = {0x11, 0x22};
    const uint16_t i1[] = {0, 1};
    const uint8_t v1[] = {0x33, 0x44};
    CHECK(page_is_ff_except(pages.data[0], i0, v0, 2));
    CHECK(page_is_ff_except(pages.data[1], i1, v1, 2));
}

static void test_hole(void) {
    PageCapture pages;
    memset(&pages, 0, sizeof(pages));
    char error[96] = {0};
    const char* lines[] = {":02001000AA55EF", ":01002000BB24", ":00000001FF", NULL};
    CHECK(feed_all(lines, &pages, error, sizeof(error)));
    CHECK(pages.count == 1);
    CHECK(pages.address[0] == 0);
    const uint16_t indexes[] = {0x10, 0x11, 0x20};
    const uint8_t values[] = {0xAA, 0x55, 0xBB};
    CHECK(page_is_ff_except(pages.data[0], indexes, values, 3));
}

static void test_type04_above_64k(void) {
    PageCapture pages;
    memset(&pages, 0, sizeof(pages));
    char error[96] = {0};
    const char* lines[] = {":020000040001F9", ":02000000ABCD86", ":00000001FF", NULL};
    CHECK(feed_all(lines, &pages, error, sizeof(error)));
    CHECK(pages.count == 1);
    CHECK(pages.address[0] == 0x10000);
    const uint16_t indexes[] = {0, 1};
    const uint8_t values[] = {0xAB, 0xCD};
    CHECK(page_is_ff_except(pages.data[0], indexes, values, 2));
}

static void test_type02(void) {
    PageCapture pages;
    memset(&pages, 0, sizeof(pages));
    char error[96] = {0};
    const char* lines[] = {":020000021000EC", ":01000000AB54", ":00000001FF", NULL};
    CHECK(feed_all(lines, &pages, error, sizeof(error)));
    CHECK(pages.count == 1);
    CHECK(pages.address[0] == 0x10000);
    CHECK(pages.data[0][0] == 0xAB);
    CHECK(pages.data[0][1] == 0xFF);
}

static void test_backwards_and_checksum(void) {
    PageCapture pages;
    memset(&pages, 0, sizeof(pages));
    char error[96] = {0};
    IntelHexParser* parser = intel_hex_parser_alloc(128u * 1024u, capture_page, &pages);
    CHECK(intel_hex_parser_feed(parser, ":020200000102F9", error, sizeof(error)));
    CHECK(!intel_hex_parser_feed(parser, ":01000000AA55", error, sizeof(error)));
    CHECK(strstr(error, "backwards") != NULL);
    intel_hex_parser_free(parser);

    memset(&pages, 0, sizeof(pages));
    error[0] = '\0';
    parser = intel_hex_parser_alloc(128u * 1024u, capture_page, &pages);
    CHECK(!intel_hex_parser_feed(parser, ":02001000AA5500", error, sizeof(error)));
    CHECK(strstr(error, "checksum") != NULL);
    intel_hex_parser_free(parser);
}

static void test_writer_crosses_64k(void) {
    LineCapture lines;
    memset(&lines, 0, sizeof(lines));
    char error[96] = {0};
    IntelHexWriter* writer = intel_hex_writer_alloc(capture_line, &lines);
    uint8_t data[16];
    for(int i = 0; i < 16; i++) data[i] = (uint8_t)(i + 1);
    CHECK(intel_hex_writer_feed(writer, 0xFFF8, data, sizeof(data), error, sizeof(error)));
    CHECK(intel_hex_writer_finish(writer, error, sizeof(error)));
    intel_hex_writer_free(writer);

    CHECK(lines.count == 4);
    CHECK(strcmp(lines.lines[0], ":08FFF8000102030405060708DD") == 0);
    CHECK(strcmp(lines.lines[1], ":020000040001F9") == 0);
    CHECK(strcmp(lines.lines[2], ":08000000090A0B0C0D0E0F1094") == 0);
    CHECK(strcmp(lines.lines[3], ":00000001FF") == 0);
}

int main(void) {
    test_page_crossing();
    test_hole();
    test_type04_above_64k();
    test_type02();
    test_backwards_and_checksum();
    test_writer_crosses_64k();
    if(failures != 0) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("intel hex tests passed\n");
    return 0;
}
