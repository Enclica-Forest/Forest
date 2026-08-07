#include "../include/graphics/font_renderer.h"
#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/font8x8.h"
#include "../include/graphics/truetype.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/mm.h"
#include "../include/vfs.h"
#include "../include/ramdisk.h"

static font_glyph_t builtin_glyphs_8x8_data[2048];
static font_glyph_t builtin_glyphs_8x16_data[2048];

// 8x16 VGA BIOS font bitmap data (CP437, classic 16-pixel-tall glyphs)
static uint8_t vga_8x16_font_bitmap[256][16] = {
    // CP437 characters 0x00-0xFF, 16 bytes per glyph (each byte = one row, MSB=left)
    // Row 0 is top of glyph
    [0x00] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // NUL
    [0x01] = {0x00,0x00,0x7E,0x81,0xA5,0x81,0x81,0xBD,0x99,0x81,0x81,0x7E,0x00,0x00,0x00,0x00}, // ☺
    [0x02] = {0x00,0x00,0x7E,0xFF,0xDB,0xFF,0xFF,0xC3,0xE7,0xFF,0xFF,0x7E,0x00,0x00,0x00,0x00}, // ☻
    [0x03] = {0x00,0x00,0x00,0x00,0x6C,0xFE,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00}, // ♥
    [0x04] = {0x00,0x00,0x00,0x00,0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00,0x00}, // ♦
    [0x05] = {0x00,0x00,0x00,0x00,0x18,0x3C,0x78,0xF8,0xF8,0x78,0x3C,0x18,0x00,0x00,0x00,0x00}, // ♣
    [0x06] = {0x00,0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x00,0x00,0x00,0x00,0x00,0x00}, // ♠
    [0x07] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // •
    [0x08] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, // ◘
    [0x09] = {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00,0x00,0x00,0x00,0x00}, // ○
    [0x0A] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF,0xFF,0xFF,0xFF,0xFF}, // ◙
    [0x0B] = {0x00,0x00,0x00,0x00,0x1C,0x36,0x22,0x22,0x36,0x1C,0x00,0x00,0x00,0x00,0x00,0x00}, // ♂
    [0x0C] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ♀
    [0x0D] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ♪
    [0x0E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ♫
    [0x0F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ☼
    [0x10] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ►
    [0x11] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ◄
    [0x12] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ↕
    [0x13] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ◆
    [0x14] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ¶
    [0x15] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // §
    [0x16] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ☒
    [0x17] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ═
    [0x18] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ▲
    [0x19] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ▼
    [0x1A] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ►
    [0x1B] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ←
    [0x1C] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ∞
    [0x1D] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // √
    [0x1E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ★
    [0x1F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ·
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    [0x21] = {0x00,0x00,0x18,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // !
    [0x22] = {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    [0x23] = {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // #
    [0x24] = {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00}, // $
    [0x25] = {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00}, // %
    [0x26] = {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // &
    [0x27] = {0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '
    [0x28] = {0x00,0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00}, // (
    [0x29] = {0x00,0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00}, // )
    [0x2A] = {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, // *
    [0x2B] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // +
    [0x2C] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00}, // ,
    [0x2D] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // -
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, // .
    [0x2F] = {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00}, // /
    [0x30] = {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 0
    [0x31] = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00}, // 1
    [0x32] = {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00}, // 2
    [0x33] = {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 3
    [0x34] = {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00}, // 4
    [0x35] = {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 5
    [0x36] = {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 6
    [0x37] = {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00}, // 7
    [0x38] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // 8
    [0x39] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00}, // 9
    [0x3A] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // :
    [0x3B] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00}, // ;
    [0x3C] = {0x00,0x00,0x00,0x0C,0x18,0x30,0x60,0xC0,0x60,0x30,0x18,0x0C,0x00,0x00,0x00,0x00}, // <
    [0x3D] = {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // =
    [0x3E] = {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00}, // >
    [0x3F] = {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, // ?
    [0x40] = {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00}, // @
    [0x41] = {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // A
    [0x42] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00}, // B
    [0x43] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00}, // C
    [0x44] = {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00}, // D
    [0x45] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00}, // E
    [0x46] = {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // F
    [0x47] = {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00}, // G
    [0x48] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // H
    [0x49] = {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // I
    [0x4A] = {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00}, // J
    [0x4B] = {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, // K
    [0x4C] = {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00}, // L
    [0x4D] = {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // M
    [0x4E] = {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, // N
    [0x4F] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // O
    [0x50] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // P
    [0x51] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00}, // Q
    [0x52] = {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, // R
    [0x53] = {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // S
    [0x54] = {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // T
    [0x55] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // U
    [0x56] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00}, // V
    [0x57] = {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00}, // W
    [0x58] = {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00}, // X
    [0x59] = {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // Y
    [0x5A] = {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00}, // Z
    [0x5B] = {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00}, // [
    [0x5C] = {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00}, // backslash
    [0x5D] = {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00}, // ]
    [0x5E] = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ^
    [0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00}, // _
    [0x60] = {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // `
    [0x61] = {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // a
    [0x62] = {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00}, // b
    [0x63] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, // c
    [0x64] = {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // d
    [0x65] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00}, // e
    [0x66] = {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // f
    [0x67] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00}, // g
    [0x68] = {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, // h
    [0x69] = {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // i
    [0x6A] = {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00}, // j
    [0x6B] = {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00}, // k
    [0x6C] = {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, // l
    [0x6D] = {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00}, // m
    [0x6E] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00}, // n
    [0x6F] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, // o
    [0x70] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // p
    [0x71] = {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00}, // q
    [0x72] = {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, // r
    [0x73] = {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00}, // s
    [0x74] = {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00}, // t
    [0x75] = {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, // u
    [0x76] = {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00}, // v
    [0x77] = {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00}, // w
    [0x78] = {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // x
    [0x79] = {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00}, // y
    [0x7A] = {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00}, // z
    [0x7B] = {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00}, // {
    [0x7C] = {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00}, // |
    [0x7D] = {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00}, // }
    [0x7E] = {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
    [0x7F] = {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00}, // DEL
    // Remaining 0x80-0xFF: filled with default rectangle pattern
};

// Fill remaining high bytes with default rectangle pattern
static bool vga_font_initialized = false;
static void ensure_vga_font_init(void) {
    if (vga_font_initialized) return;
    vga_font_initialized = true;
    for (uint32_t i = 0x80; i < 256; i++) {
        bool already_set = false;
        for (int r = 0; r < 16; r++) {
            if (vga_8x16_font_bitmap[i][r] != 0) { already_set = true; break; }
        }
        if (!already_set) {
            // Fill with a small filled block character for high-byte glyphs
            vga_8x16_font_bitmap[i][3]  = 0x3C;
            vga_8x16_font_bitmap[i][4]  = 0x42;
            vga_8x16_font_bitmap[i][5]  = 0xA5;
            vga_8x16_font_bitmap[i][6]  = 0x81;
            vga_8x16_font_bitmap[i][7]  = 0xA5;
            vga_8x16_font_bitmap[i][8]  = 0x99;
            vga_8x16_font_bitmap[i][9]  = 0x42;
            vga_8x16_font_bitmap[i][10] = 0x3C;
        }
    }
}

// Glyph bitmap storage for 8x16 font (each glyph needs 16 bytes)
static uint8_t vga_8x16_glyph_bitmaps[256][16];

// Font cache for loaded TTF fonts (keyed by filename hash)
#define FONT_CACHE_SIZE 8
static struct {
    char path[128];
    font_t* font;
} font_cache[FONT_CACHE_SIZE];
static uint32_t font_cache_count = 0;

// Font renderer state
static struct {
    bool initialized;
    font_t* system_font;
    font_t* builtin_fonts[2];
    uint32_t font_count;
    bool pending_config_load;
    bool using_fallback_font;
} font_renderer_state = {
    .initialized = false,
    .system_font = NULL,
    .font_count = 0,
    .pending_config_load = false,
    .using_fallback_font = false
};

static graphics_result_t create_builtin_8x8_font(void);
static graphics_result_t create_builtin_8x16_font(void);
static graphics_result_t render_glyph(font_t* font, font_glyph_t* glyph,
                                     graphics_surface_t* surface, int32_t x, int32_t y,
                                     const text_style_t* style);
static graphics_result_t render_glyph_coverage(font_t* font, uint8_t* coverage,
                                               uint8_t width, uint8_t height,
                                               int8_t bearing_x, int8_t bearing_y,
                                              graphics_surface_t* surface, int32_t x, int32_t y,
                                              const text_style_t* style);
static void apply_text_effects(graphics_surface_t* surface, const graphics_rect_t* bounds,
                              const text_style_t* style);
static uint32_t find_glyph_index(font_t* font, uint32_t codepoint);
static graphics_result_t load_system_font_from_config(void);
static void maybe_load_system_font_from_config(void);
static font_glyph_t* get_or_rasterize_glyph(font_t* font, uint32_t codepoint, uint32_t* glyph_index);
static font_t* font_cache_lookup(const char* path);
static void font_cache_insert(const char* path, font_t* font);

graphics_result_t font_renderer_init(void) {
    debuglog(DEBUG_INFO, "Initializing font renderer...\n");
    
    if (font_renderer_state.initialized) {
        debuglog(DEBUG_WARN, "Font renderer already initialized\n");
        return GRAPHICS_SUCCESS;
    }
    
    memset(&font_renderer_state, 0, sizeof(font_renderer_state));
    memset(font_cache, 0, sizeof(font_cache));
    font_cache_count = 0;

    ensure_vga_font_init();
    
    // Initialize built-in glyph data for 8x8 font using font8x8 system
    uint32_t glyph_index = 0;
    for (uint32_t block_idx = 0; block_idx < font8x8_get_block_count(); block_idx++) {
        const font8x8_block_t* block = font8x8_get_block_info(block_idx);
        if (!block || glyph_index >= 2048) break;
        
        for (uint32_t char_idx = 0; char_idx < block->count && glyph_index < 2048; char_idx++) {
            uint32_t codepoint = block->unicode_start + char_idx;
            builtin_glyphs_8x8_data[glyph_index].codepoint = codepoint;
            builtin_glyphs_8x8_data[glyph_index].width = 8;
            builtin_glyphs_8x8_data[glyph_index].height = 8;
            builtin_glyphs_8x8_data[glyph_index].bearing_x = 0;
            builtin_glyphs_8x8_data[glyph_index].bearing_y = 8;
            builtin_glyphs_8x8_data[glyph_index].advance = 8;
            builtin_glyphs_8x8_data[glyph_index].bitmap = (uint8_t*)&(block->data[char_idx][0]);
            glyph_index++;
        }
    }
    
    // Create built-in 8x8 font
    graphics_result_t result = create_builtin_8x8_font();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "Failed to create built-in 8x8 font\n");
        font_renderer_state.initialized = true;
        font_renderer_state.using_fallback_font = true;
        debuglog(DEBUG_WARN, "Font renderer running in degraded mode (no 8x8 font)\n");
        return GRAPHICS_SUCCESS;
    }

    result = create_builtin_8x16_font();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "Failed to create built-in 8x16 font, using 8x8 only\n");
    }
    
    font_renderer_state.initialized = true;
    font_renderer_state.pending_config_load = true;

    if (ramdisk_file_count() == 0) {
        debuglog(DEBUG_INFO, "Initrd not ready; deferring system font load\n");
    }

    maybe_load_system_font_from_config();

    debuglog(DEBUG_INFO, "Font renderer initialized successfully (builtin: 8x8=%s, 8x16=%s)\n",
             font_renderer_state.builtin_fonts[0] ? "yes" : "no",
             font_renderer_state.builtin_fonts[1] ? "yes" : "no");

    return GRAPHICS_SUCCESS;
}

void font_renderer_on_initrd_ready(void) {
    font_renderer_state.pending_config_load = true;
    if (font_renderer_state.initialized) {
        maybe_load_system_font_from_config();
    }
}

graphics_result_t font_renderer_shutdown(void) {
    if (!font_renderer_state.initialized) {
        return GRAPHICS_SUCCESS;
    }
    
    debuglog(DEBUG_INFO, "Shutting down font renderer...\n");
    
    // Clear font cache
    for (uint32_t i = 0; i < font_cache_count; i++) {
        if (font_cache[i].font) {
            font_unload(font_cache[i].font);
            font_cache[i].font = NULL;
        }
        font_cache[i].path[0] = '\0';
    }
    font_cache_count = 0;

    // Unload non-builtin system fonts (TTF loaded from files)
    if (font_renderer_state.system_font &&
        font_renderer_state.system_font != font_renderer_state.builtin_fonts[0] &&
        font_renderer_state.system_font != font_renderer_state.builtin_fonts[1]) {
        font_unload(font_renderer_state.system_font);
    }
    font_renderer_state.system_font = NULL;

    // Reset builtin font pointers (don't free, they're static)
    font_renderer_state.builtin_fonts[0] = NULL;
    font_renderer_state.builtin_fonts[1] = NULL;
    
    font_renderer_state.pending_config_load = false;
    font_renderer_state.using_fallback_font = false;
    font_renderer_state.initialized = false;
    
    debuglog(DEBUG_INFO, "Font renderer shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

bool font_renderer_is_initialized(void) {
    return font_renderer_state.initialized;
}

graphics_result_t font_load_builtin(const char* name, uint8_t size, font_t** font) {
    if (!name || !font || !font_renderer_state.initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    if (strcmp(name, "system-8x8") == 0 && size == 8) {
        *font = font_renderer_state.builtin_fonts[0];
        return GRAPHICS_SUCCESS;
    }

    if (strcmp(name, "system-8x16") == 0 && (size == 16 || size == 0)) {
        if (font_renderer_state.builtin_fonts[1]) {
            *font = font_renderer_state.builtin_fonts[1];
            return GRAPHICS_SUCCESS;
        }
        *font = font_renderer_state.builtin_fonts[0];
        return GRAPHICS_SUCCESS;
    }

    if (strcmp(name, "Inter") == 0) {
        font_t* cached = font_cache_lookup("/usr/share/fonts/trim.ttf");
        if (cached) { *font = cached; return GRAPHICS_SUCCESS; }
        return font_load_from_file("/usr/share/fonts/trim.ttf", "Inter", size, font);
    }

    if (strcmp(name, "Default") == 0) {
        font_t* cached = font_cache_lookup("/usr/share/fonts/Tuffy_Bold_Italic.ttf");
        if (cached) { *font = cached; return GRAPHICS_SUCCESS; }
        return font_load_from_file("/usr/share/fonts/Tuffy_Bold_Italic.ttf", "Default", size, font);
    }

    debuglog(DEBUG_WARN, "Built-in font '%s' size %u not found\n", name, size);
    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t font_unload(font_t* font) {
    if (!font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    if (font == font_renderer_state.builtin_fonts[0] ||
        font == font_renderer_state.builtin_fonts[1]) {
        return GRAPHICS_SUCCESS;
    }

    if (font->type == FONT_TYPE_VECTOR && font->font_data) {
        ttf_font_t* ttf = (ttf_font_t*)font->font_data;
        ttf_unload(ttf);
        font->font_data = NULL;
    }

    if (font->glyphs && font->glyphs != builtin_glyphs_8x8_data &&
        font->glyphs != builtin_glyphs_8x16_data) {
        for (uint32_t i = 0; i < font->num_glyphs; i++) {
            if (font->glyphs[i].bitmap) {
                kfree(font->glyphs[i].bitmap);
            }
        }
        kfree(font->glyphs);
    }
    if (font->codepoint_map) {
        kfree(font->codepoint_map);
    }
    if (font->advance_table) {
        kfree(font->advance_table);
    }

    kfree(font);
    return GRAPHICS_SUCCESS;
}

graphics_result_t font_measure_text(font_t* font, const char* text,
                                   uint32_t* width, uint32_t* height) {
    if (!font || !text || !width || !height) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    *width = 0;
    *height = font->metrics.height;

    uint32_t current_width = 0;
    uint32_t max_width = 0;
    uint32_t line_count = 1;

    const char* p = text;
    while (*p) {
        if (*p == '\n') {
            if (current_width > max_width) {
                max_width = current_width;
            }
            current_width = 0;
            line_count++;
            p++;
            continue;
        }

        uint32_t bytes_consumed;
        uint32_t codepoint = utf8_decode(p, &bytes_consumed);

        if (font->is_fixed_width) {
            current_width += font->fixed_width;
        } else if (font->type == FONT_TYPE_VECTOR && font->font_data) {
            // TTF font - get advance from TTF glyph metrics
            ttf_font_t* ttf = (ttf_font_t*)font->font_data;
            uint16_t glyph_id = ttf_get_glyph_index(ttf, codepoint);
            if (glyph_id != 0 || codepoint == 0) {
                int16_t advance = ttf_get_glyph_advance(ttf, glyph_id, font->size);
                current_width += (advance > 0) ? advance : font->size;
            } else {
                current_width += font->size;  // Default for missing glyphs
            }
        } else {
            // Bitmap font - find glyph and get advance
            uint32_t glyph_index = find_glyph_index(font, codepoint);
            if (glyph_index < font->num_glyphs) {
                current_width += font->glyphs[glyph_index].advance;
            } else {
                current_width += 8;  // Default advance
            }
        }

        p += bytes_consumed;
    }

    if (current_width > max_width) {
        max_width = current_width;
    }

    *width = max_width;
    *height = line_count * font->metrics.height;

    return GRAPHICS_SUCCESS;
}

graphics_result_t font_render_text(font_t* font, graphics_surface_t* surface,
                                  int32_t x, int32_t y, const char* text,
                                  const text_style_t* style) {
    if (!font || !surface || !text) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    text_style_t default_style = DEFAULT_TEXT_STYLE;
    if (!style) {
        style = &default_style;
    }
    
    int32_t current_x = x;
    int32_t current_y = y;
    
    const char* p = text;
    while (*p) {
        if (*p == '\n') {
            current_x = x;
            current_y += font->metrics.height;
            p++;
            continue;
        }
        
        if (*p == '\r') {
            current_x = x;
            p++;
            continue;
        }
        
        if (*p == '\t') {
            // Tab to next 4-character boundary
            uint32_t tab_width = font->is_fixed_width ? font->fixed_width * 4 : 32;
            current_x = ((current_x - x + tab_width) / tab_width) * tab_width + x;
            p++;
            continue;
        }
        
        uint32_t bytes_consumed;
        uint32_t codepoint = utf8_decode(p, &bytes_consumed);
        
        graphics_result_t result = font_render_char(font, surface, current_x, current_y, 
                                                   codepoint, style);
        if (result != GRAPHICS_SUCCESS) {
            return result;
        }
        
        // Advance cursor
        if (font->is_fixed_width) {
            current_x += font->fixed_width;
        } else if (font->type == FONT_TYPE_VECTOR && font->font_data) {
            // TTF font - get advance from TTF glyph metrics
            ttf_font_t* ttf = (ttf_font_t*)font->font_data;
            uint16_t glyph_id = ttf_get_glyph_index(ttf, codepoint);
            int16_t advance = ttf_get_glyph_advance(ttf, glyph_id, font->size);
            current_x += (advance > 0) ? advance : font->size;
        } else {
            // Bitmap font
            uint32_t glyph_index = find_glyph_index(font, codepoint);
            if (glyph_index < font->num_glyphs) {
                current_x += font->glyphs[glyph_index].advance;
            } else {
                current_x += 8;  // Default advance
            }
        }

        p += bytes_consumed;
    }
    
    return GRAPHICS_SUCCESS;
}

graphics_result_t font_render_char(font_t* font, graphics_surface_t* surface,
                                  int32_t x, int32_t y, uint32_t codepoint,
                                  const text_style_t* style) {
    if (!font || !surface) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    text_style_t default_style = DEFAULT_TEXT_STYLE;
    if (!style) {
        style = &default_style;
    }

    // Handle TTF (vector) fonts
    if (font->type == FONT_TYPE_VECTOR && font->font_data) {
        ttf_font_t* ttf = (ttf_font_t*)font->font_data;

        // Get glyph index from codepoint
        uint16_t glyph_id = ttf_get_glyph_index(ttf, codepoint);

        // Check for missing glyph (glyph_id 0 is .notdef, only valid for codepoint 0)
        if (glyph_id == 0 && codepoint != 0) {
            // Glyph not found in TTF - fall back to 8x8 bitmap font
            if (font_renderer_state.builtin_fonts[0]) {
                return font_render_char(font_renderer_state.builtin_fonts[0],
                                        surface, x, y, codepoint, style);
            }
            return GRAPHICS_ERROR_NOT_SUPPORTED;
        }

        // Rasterize the glyph at the current point size
        uint8_t width = 0, height = 0;
        int8_t bearing_x = 0, bearing_y = 0;
        bool antialias = true;

        uint8_t* coverage = ttf_rasterize_glyph(ttf, glyph_id, font->size,
                                                 &width, &height,
                                                 &bearing_x, &bearing_y,
                                                 antialias);

        if (!coverage && (width > 0 || height > 0)) {
            // Rasterization failed but glyph has dimensions - error
            return GRAPHICS_ERROR_OUT_OF_MEMORY;
        }

        if (coverage) {
            // Render the coverage bitmap with antialiasing
            graphics_result_t result = render_glyph_coverage(font, coverage, width, height,
                                                              bearing_x, bearing_y,
                                                              surface, x, y, style);
            kfree(coverage);
            if (result != GRAPHICS_SUCCESS) {
                return result;
            }
        }
        // else: empty glyph (like space) - nothing to render

        return GRAPHICS_SUCCESS;
    }

    // Handle bitmap fonts (8x8, etc.)
    uint32_t glyph_index;
    font_glyph_t* glyph = get_or_rasterize_glyph(font, codepoint, &glyph_index);
    if (!glyph) {
        // Use space character as fallback
        glyph = get_or_rasterize_glyph(font, ' ', &glyph_index);
        if (!glyph) {
            return GRAPHICS_ERROR_NOT_SUPPORTED;
        }
    }

    return render_glyph(font, glyph, surface, x, y, style);
}

graphics_result_t font_get_system_font(font_t** font) {
    if (!font || !font_renderer_state.initialized) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    maybe_load_system_font_from_config();
    
    if (!font_renderer_state.system_font) {
        // Prefer 8x16 over 8x8 for better readability
        if (font_renderer_state.builtin_fonts[1]) {
            font_renderer_state.system_font = font_renderer_state.builtin_fonts[1];
        } else {
            font_renderer_state.system_font = font_renderer_state.builtin_fonts[0];
        }
    }
    
    *font = font_renderer_state.system_font;
    return GRAPHICS_SUCCESS;
}

graphics_result_t font_set_system_font(font_t* font) {
    if (!font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    font_renderer_state.system_font = font;
    return GRAPHICS_SUCCESS;
}

// Helper function implementations
static graphics_result_t create_builtin_8x8_font(void) {
    font_t* font = kmalloc(sizeof(font_t));
    if (!font) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    memset(font, 0, sizeof(font_t));
    
    strcpy(font->name, "system-8x8");
    font->size = 8;
    font->type = FONT_TYPE_BITMAP;
    font->metrics.ascent = 6;
    font->metrics.descent = 2;
    font->metrics.line_gap = 0;
    font->metrics.max_advance = 8;
    font->metrics.height = 8;
    font->num_glyphs = 512;
    font->glyphs = builtin_glyphs_8x8_data;
    font->is_fixed_width = true;
    font->fixed_width = 8;
    
    font_renderer_state.builtin_fonts[0] = font;
    font_renderer_state.font_count++;
    
    debuglog(DEBUG_INFO, "Created built-in 8x8 font\n");
    return GRAPHICS_SUCCESS;
}

static graphics_result_t create_builtin_8x16_font(void) {
    ensure_vga_font_init();

    for (uint32_t i = 0; i < 256; i++) {
        builtin_glyphs_8x16_data[i].codepoint = i;
        builtin_glyphs_8x16_data[i].width = 8;
        builtin_glyphs_8x16_data[i].height = 16;
        builtin_glyphs_8x16_data[i].bearing_x = 0;
        builtin_glyphs_8x16_data[i].bearing_y = 14;
        builtin_glyphs_8x16_data[i].advance = 8;
        builtin_glyphs_8x16_data[i].bitmap = vga_8x16_glyph_bitmaps[i];
        memcpy(vga_8x16_glyph_bitmaps[i], vga_8x16_font_bitmap[i], 16);
    }

    font_t* font = kmalloc(sizeof(font_t));
    if (!font) {
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }
    
    memset(font, 0, sizeof(font_t));
    
    strcpy(font->name, "system-8x16");
    font->size = 16;
    font->type = FONT_TYPE_BITMAP;
    font->metrics.ascent = 12;
    font->metrics.descent = 4;
    font->metrics.line_gap = 0;
    font->metrics.max_advance = 8;
    font->metrics.height = 16;
    font->num_glyphs = 256;
    font->glyphs = builtin_glyphs_8x16_data;
    font->is_fixed_width = true;
    font->fixed_width = 8;
    
    font_renderer_state.builtin_fonts[1] = font;
    font_renderer_state.font_count++;
    
    debuglog(DEBUG_INFO, "Created built-in 8x16 VGA font\n");
    return GRAPHICS_SUCCESS;
}

static graphics_result_t render_glyph(font_t* font, font_glyph_t* glyph, 
                                     graphics_surface_t* surface, int32_t x, int32_t y,
                                     const text_style_t* style) {
    (void)font;
    if (!glyph || !glyph->bitmap || !surface || !surface->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }
    
    // Calculate glyph position - for 8x8 bitmap font, use direct positioning
    int32_t glyph_x = x;
    int32_t glyph_y = y;
    
    // Convert colors to pixel format
    uint32_t fg_pixel = graphics_color_to_pixel(style->foreground, surface->format);
    uint32_t bg_pixel = graphics_color_to_pixel(style->background, surface->format);
    
    // Render bitmap glyph (8x8 for built-in font)
    for (int dy = 0; dy < glyph->height; dy++) {
        if (glyph_y + dy < 0 || glyph_y + dy >= (int32_t)surface->height) {
            continue;
        }
        
        // Safety check for bitmap access
        if (!glyph->bitmap || dy >= glyph->height) {
            continue;
        }
        
        for (int dx = 0; dx < glyph->width; dx++) {
            if (glyph_x + dx < 0 || glyph_x + dx >= (int32_t)surface->width) {
                continue;
            }
            
            // Extract pixel from bitmap data  
            uint8_t row = glyph->bitmap[dy];
            // Fix horizontal flipping: check bits from LSB (right) to MSB (left)
            bool pixel_set = (row & (0x01 << dx)) != 0;
            
            if (pixel_set || style->has_background) {
                uint32_t pixel_value = pixel_set ? fg_pixel : bg_pixel;
                
                // Write pixel based on surface format
                uint32_t pixel_offset;
                switch (surface->format) {
                    case PIXEL_FORMAT_RGB_565:
                    case PIXEL_FORMAT_RGB_555: {
                        pixel_offset = (glyph_y + dy) * surface->pitch + (glyph_x + dx) * 2;
                        uint16_t* pixel_ptr = (uint16_t*)((uint8_t*)surface->pixels + pixel_offset);
                        *pixel_ptr = (uint16_t)pixel_value;
                        break;
                    }
                    case PIXEL_FORMAT_RGB_888: {
                        pixel_offset = (glyph_y + dy) * surface->pitch + (glyph_x + dx) * 3;
                        uint8_t* pixel_ptr = (uint8_t*)surface->pixels + pixel_offset;
                        pixel_ptr[0] = pixel_value & 0xFF;
                        pixel_ptr[1] = (pixel_value >> 8) & 0xFF;
                        pixel_ptr[2] = (pixel_value >> 16) & 0xFF;
                        break;
                    }
                    case PIXEL_FORMAT_BGR_888: {
                        pixel_offset = (glyph_y + dy) * surface->pitch + (glyph_x + dx) * 3;
                        uint8_t* pixel_ptr = (uint8_t*)surface->pixels + pixel_offset;
                        pixel_ptr[0] = (pixel_value >> 16) & 0xFF;
                        pixel_ptr[1] = (pixel_value >> 8) & 0xFF;
                        pixel_ptr[2] = pixel_value & 0xFF;
                        break;
                    }
                    case PIXEL_FORMAT_RGBA_8888:
                    case PIXEL_FORMAT_BGRA_8888: {
                        pixel_offset = (glyph_y + dy) * surface->pitch + (glyph_x + dx) * 4;
                        uint32_t* pixel_ptr = (uint32_t*)((uint8_t*)surface->pixels + pixel_offset);
                        *pixel_ptr = pixel_value;
                        break;
                    }
                    default:
                        return GRAPHICS_ERROR_NOT_SUPPORTED;
                }
            }
        }
    }
    
    // Apply text effects
    if (style->underline || style->strikethrough) {
        graphics_rect_t effect_bounds = {
            glyph_x, glyph_y, glyph->width, glyph->height
        };
        apply_text_effects(surface, &effect_bounds, style);
    }
    
    return GRAPHICS_SUCCESS;
}

static void apply_text_effects(graphics_surface_t* surface, const graphics_rect_t* bounds,
                              const text_style_t* style) {
    if (!surface || !bounds || !style) {
        return;
    }
    
    uint32_t fg_pixel = graphics_color_to_pixel(style->foreground, surface->format);
    
    // Draw underline
    if (style->underline) {
        int32_t underline_y = bounds->y + bounds->height - 1;
        if (underline_y >= 0 && underline_y < (int32_t)surface->height) {
            for (uint32_t x = bounds->x; x < bounds->x + bounds->width; x++) {
                if (x < surface->width) {
                    switch (surface->format) {
                        case PIXEL_FORMAT_RGB_565:
                        case PIXEL_FORMAT_RGB_555: {
                            uint32_t offset = underline_y * surface->pitch + x * 2;
                            uint16_t* pixel_ptr = (uint16_t*)((uint8_t*)surface->pixels + offset);
                            *pixel_ptr = (uint16_t)fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_RGB_888: {
                            uint32_t offset = underline_y * surface->pitch + x * 3;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            pixel_ptr[0] = fg_pixel & 0xFF;
                            pixel_ptr[1] = (fg_pixel >> 8) & 0xFF;
                            pixel_ptr[2] = (fg_pixel >> 16) & 0xFF;
                            break;
                        }
                        case PIXEL_FORMAT_BGR_888: {
                            uint32_t offset = underline_y * surface->pitch + x * 3;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            pixel_ptr[0] = (fg_pixel >> 16) & 0xFF;
                            pixel_ptr[1] = (fg_pixel >> 8) & 0xFF;
                            pixel_ptr[2] = fg_pixel & 0xFF;
                            break;
                        }
                        case PIXEL_FORMAT_RGBA_8888:
                        case PIXEL_FORMAT_BGRA_8888: {
                            uint32_t offset = underline_y * surface->pitch + x * 4;
                            uint32_t* pixel_ptr = (uint32_t*)((uint8_t*)surface->pixels + offset);
                            *pixel_ptr = fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_INDEXED_8: {
                            uint32_t offset = underline_y * surface->pitch + x;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            *pixel_ptr = (uint8_t)fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_TEXT_MODE:
                        default:
                            // Skip unsupported formats
                            break;
                    }
                }
            }
        }
    }
    
    // Draw strikethrough
    if (style->strikethrough) {
        int32_t strike_y = bounds->y + bounds->height / 2;
        if (strike_y >= 0 && strike_y < (int32_t)surface->height) {
            for (uint32_t x = bounds->x; x < bounds->x + bounds->width; x++) {
                if (x < surface->width) {
                    switch (surface->format) {
                        case PIXEL_FORMAT_RGB_565:
                        case PIXEL_FORMAT_RGB_555: {
                            uint32_t offset = strike_y * surface->pitch + x * 2;
                            uint16_t* pixel_ptr = (uint16_t*)((uint8_t*)surface->pixels + offset);
                            *pixel_ptr = (uint16_t)fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_RGB_888: {
                            uint32_t offset = strike_y * surface->pitch + x * 3;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            pixel_ptr[0] = fg_pixel & 0xFF;
                            pixel_ptr[1] = (fg_pixel >> 8) & 0xFF;
                            pixel_ptr[2] = (fg_pixel >> 16) & 0xFF;
                            break;
                        }
                        case PIXEL_FORMAT_BGR_888: {
                            uint32_t offset = strike_y * surface->pitch + x * 3;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            pixel_ptr[0] = (fg_pixel >> 16) & 0xFF;
                            pixel_ptr[1] = (fg_pixel >> 8) & 0xFF;
                            pixel_ptr[2] = fg_pixel & 0xFF;
                            break;
                        }
                        case PIXEL_FORMAT_RGBA_8888:
                        case PIXEL_FORMAT_BGRA_8888: {
                            uint32_t offset = strike_y * surface->pitch + x * 4;
                            uint32_t* pixel_ptr = (uint32_t*)((uint8_t*)surface->pixels + offset);
                            *pixel_ptr = fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_INDEXED_8: {
                            uint32_t offset = strike_y * surface->pitch + x;
                            uint8_t* pixel_ptr = (uint8_t*)surface->pixels + offset;
                            *pixel_ptr = (uint8_t)fg_pixel;
                            break;
                        }
                        case PIXEL_FORMAT_TEXT_MODE:
                        default:
                            // Skip unsupported formats
                            break;
                    }
                }
            }
        }
    }
}

static uint32_t find_glyph_index(font_t* font, uint32_t codepoint) {
    if (!font || !font->glyphs) {
        return 0;
    }
    
    // Always do linear search to find the correct glyph
    for (uint32_t i = 0; i < font->num_glyphs; i++) {
        if (font->glyphs[i].codepoint == codepoint) {
            return i;
        }
    }
    
    // Fallback to space character (codepoint 32) if not found
    for (uint32_t i = 0; i < font->num_glyphs; i++) {
        if (font->glyphs[i].codepoint == 32) {
            return i;
        }
    }
    
    return 0; // Return first glyph (usually space) if not found
}

// UTF-8 utility functions
uint32_t utf8_decode(const char* str, uint32_t* bytes_consumed) {
    if (!str || !bytes_consumed) {
        *bytes_consumed = 0;
        return 0;
    }
    
    uint8_t first = (uint8_t)str[0];
    
    if ((first & 0x80) == 0) {
        // ASCII (0xxxxxxx)
        *bytes_consumed = 1;
        return first;
    } else if ((first & 0xE0) == 0xC0) {
        // 2-byte sequence (110xxxxx 10xxxxxx)
        if ((str[1] & 0xC0) == 0x80) {
            *bytes_consumed = 2;
            return ((first & 0x1F) << 6) | (str[1] & 0x3F);
        }
    } else if ((first & 0xF0) == 0xE0) {
        // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
        if ((str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80) {
            *bytes_consumed = 3;
            return ((first & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
        }
    } else if ((first & 0xF8) == 0xF0) {
        // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
        if ((str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80 && (str[3] & 0xC0) == 0x80) {
            *bytes_consumed = 4;
            return ((first & 0x07) << 18) | ((str[1] & 0x3F) << 12) | 
                   ((str[2] & 0x3F) << 6) | (str[3] & 0x3F);
        }
    }
    
    // Invalid UTF-8, treat as single byte
    *bytes_consumed = 1;
    return first;
}

uint32_t utf8_strlen(const char* str) {
    if (!str) {
        return 0;
    }

    uint32_t length = 0;
    const char* p = str;

    while (*p) {
        uint32_t bytes_consumed;
        utf8_decode(p, &bytes_consumed);
        p += bytes_consumed;
        length++;
    }

    return length;
}

// =============================================================================
// TrueType/OpenType Support Functions
// =============================================================================

graphics_result_t font_detect_format(const void* data, size_t size, font_format_t* format) {
    if (!data || size < 4 || !format) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    const uint8_t* bytes = (const uint8_t*)data;

    // Check for TTF/OTF magic numbers
    // TrueType: 0x00010000 or 'true'
    // OpenType with TrueType outlines: 0x00010000
    // OpenType with CFF: 'OTTO'
    if (size >= 4) {
        uint32_t magic = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];

        // TrueType / OpenType TTF
        if (magic == 0x00010000 ||
            magic == 0x74727565) {  // 'true'
            *format = FONT_FORMAT_TTF;
            return GRAPHICS_SUCCESS;
        }

        // OpenType with CFF outlines
        if (magic == 0x4F54544F) {  // 'OTTO'
            *format = FONT_FORMAT_OTF;
            return GRAPHICS_SUCCESS;
        }

        // TrueType collection
        if (magic == 0x74746366) {  // 'ttcf'
            *format = FONT_FORMAT_TTF;
            return GRAPHICS_SUCCESS;
        }
    }

    // Check for BDF format (starts with "STARTFONT")
    if (size >= 9 && memcmp(data, "STARTFONT", 9) == 0) {
        *format = FONT_FORMAT_BDF;
        return GRAPHICS_SUCCESS;
    }

    // Check for PCF format (magic: 0x01666370 little-endian)
    if (size >= 4) {
        uint32_t pcf_magic = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        if (pcf_magic == 0x70636601) {  // "\1pcf"
            *format = FONT_FORMAT_PCF;
            return GRAPHICS_SUCCESS;
        }
    }

    return GRAPHICS_ERROR_NOT_SUPPORTED;
}

graphics_result_t font_load_from_memory(const void* data, size_t size,
                                        const char* name, uint8_t point_size, font_t** out_font) {
    if (!data || size == 0 || !out_font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    // Detect font format
    font_format_t format;
    graphics_result_t result = font_detect_format(data, size, &format);
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_ERROR, "font_load_from_memory: Unknown font format\n");
        return result;
    }

    // Only TTF/OTF supported for now
    if (format != FONT_FORMAT_TTF && format != FONT_FORMAT_OTF) {
        debuglog(DEBUG_ERROR, "font_load_from_memory: Only TTF/OTF formats supported\n");
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    // Load TTF font
    ttf_font_t* ttf = NULL;
    ttf_result_t ttf_result = ttf_load_from_memory(data, size, &ttf);
    if (ttf_result != TTF_SUCCESS || !ttf) {
        debuglog(DEBUG_ERROR, "font_load_from_memory: Failed to parse TTF data\n");
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    // Create font_t wrapper
    font_t* font = kmalloc(sizeof(font_t));
    if (!font) {
        ttf_unload(ttf);
        return GRAPHICS_ERROR_OUT_OF_MEMORY;
    }

    memset(font, 0, sizeof(font_t));

    // Copy name or generate one
    if (name && name[0]) {
        strncpy(font->name, name, sizeof(font->name) - 1);
    } else {
        strcpy(font->name, "TTF Font");
    }

    font->size = point_size > 0 ? point_size : 12;  // Default to 12pt
    font->type = FONT_TYPE_VECTOR;
    font->font_data = ttf;
    font->data_size = size;

    // Calculate metrics from TTF
    uint16_t upem = ttf->units_per_em;
    if (upem == 0) upem = 1000;

    // Scale metrics to point size
    int32_t scale_ascent = ((int32_t)ttf->ascender * font->size + upem / 2) / upem;
    int32_t scale_descent = ((int32_t)(-ttf->descender) * font->size + upem / 2) / upem;
    int32_t scale_line_gap = ((int32_t)ttf->line_gap * font->size + upem / 2) / upem;

    font->metrics.ascent = (uint8_t)(scale_ascent > 255 ? 255 : scale_ascent);
    font->metrics.descent = (uint8_t)(scale_descent > 255 ? 255 : scale_descent);
    font->metrics.line_gap = (uint8_t)(scale_line_gap > 255 ? 255 : scale_line_gap);
    font->metrics.height = font->metrics.ascent + font->metrics.descent + font->metrics.line_gap;
    font->metrics.max_advance = font->size;  // Approximate

    font->is_fixed_width = false;  // TTF fonts are typically proportional
    font->num_glyphs = 0;  // Glyphs are rasterized on demand
    font->glyphs = NULL;

    *out_font = font;
    debuglog(DEBUG_INFO, "Loaded TTF font '%s' at %u pt (ascent=%u, descent=%u, height=%u)\n",
             font->name, font->size, font->metrics.ascent, font->metrics.descent, font->metrics.height);

    return GRAPHICS_SUCCESS;
}

graphics_result_t font_load_from_file(const char* filename,
                                      const char* name, uint8_t point_size, font_t** out_font) {
    if (!filename || !out_font) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    debuglog(DEBUG_INFO, "font_load_from_file: Loading '%s'\n", filename);

    // Read file using VFS
    const uint8_t* data = NULL;
    uint32_t size = 0;

    if (!vfs_read_file(filename, &data, &size)) {
        debuglog(DEBUG_WARN, "font_load_from_file: Failed to read '%s'\n", filename);
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    if (!data || size == 0) {
        debuglog(DEBUG_WARN, "font_load_from_file: Empty file '%s'\n", filename);
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    // Load from memory - this will copy the data internally
    graphics_result_t result = font_load_from_memory(data, size, name, point_size, out_font);

    // Note: vfs_read_file returns a pointer to VFS-managed memory,
    // the TTF loader makes its own copy, so we don't free it here

    font_cache_insert(filename, *out_font);
    return result;
}

static uint32_t font_path_hash(const char* path) {
    uint32_t h = 5381;
    while (*path) {
        h = h * 33 + (uint8_t)*path++;
    }
    return h;
}

static font_t* font_cache_lookup(const char* path) {
    for (uint32_t i = 0; i < font_cache_count; i++) {
        if (strcmp(font_cache[i].path, path) == 0) {
            return font_cache[i].font;
        }
    }
    return NULL;
}

static void font_cache_insert(const char* path, font_t* font) {
    if (!path || !font) return;
    if (font_cache_count >= FONT_CACHE_SIZE) {
        uint32_t victim = font_path_hash(path) % FONT_CACHE_SIZE;
        if (font_cache[victim].font) {
            font_unload(font_cache[victim].font);
        }
        strncpy(font_cache[victim].path, path, sizeof(font_cache[victim].path) - 1);
        font_cache[victim].path[sizeof(font_cache[victim].path) - 1] = '\0';
        font_cache[victim].font = font;
        return;
    }
    strncpy(font_cache[font_cache_count].path, path, sizeof(font_cache[font_cache_count].path) - 1);
    font_cache[font_cache_count].path[sizeof(font_cache[font_cache_count].path) - 1] = '\0';
    font_cache[font_cache_count].font = font;
    font_cache_count++;
}

bool font_renderer_health_check(void) {
    if (!font_renderer_state.initialized) return false;

    // Verify at least one builtin font exists and is usable
    font_t* f = font_renderer_state.builtin_fonts[0];
    if (!f) f = font_renderer_state.builtin_fonts[1];
    if (!f) return false;

    // Verify font glyph data is accessible
    if (!f->glyphs) return false;

    // Check that at least space (0x20) glyph has valid bitmap
    for (uint32_t i = 0; i < f->num_glyphs; i++) {
        if (f->glyphs[i].codepoint == ' ' && f->glyphs[i].bitmap) {
            return true;
        }
    }

    return f->num_glyphs > 0;
}

// Render an 8-bit coverage (grayscale) bitmap with alpha blending
static graphics_result_t render_glyph_coverage(font_t* font, uint8_t* coverage,
                                               uint8_t width, uint8_t height,
                                               int8_t bearing_x, int8_t bearing_y,
                                               graphics_surface_t* surface, int32_t x, int32_t y,
                                               const text_style_t* style) {
    (void)font;
    if (!coverage || !surface || !surface->pixels) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    // Calculate glyph position
    // x is the pen position, bearing_x is offset from pen to left edge of glyph
    // y is the baseline, bearing_y is offset from baseline to top of glyph
    int32_t glyph_x = x + bearing_x;
    int32_t glyph_y = y - bearing_y - 8;  // Subtract because y increases downward, offset TTF fonts up

    // Extract foreground color components
    uint8_t fg_r = style->foreground.r;
    uint8_t fg_g = style->foreground.g;
    uint8_t fg_b = style->foreground.b;

    // Render each pixel with alpha blending
    for (int32_t dy = 0; dy < height; dy++) {
        int32_t py = glyph_y + dy;
        if (py < 0 || py >= (int32_t)surface->height) {
            continue;
        }

        for (int32_t dx = 0; dx < width; dx++) {
            int32_t px = glyph_x + dx;
            if (px < 0 || px >= (int32_t)surface->width) {
                continue;
            }

            uint8_t alpha = coverage[dy * width + dx];
            if (alpha == 0) {
                continue;  // Fully transparent
            }

            uint32_t pixel_offset;
            uint8_t bg_r, bg_g, bg_b;

            // Read existing pixel and blend
            switch (surface->format) {
                case PIXEL_FORMAT_RGBA_8888:
                case PIXEL_FORMAT_BGRA_8888: {
                    pixel_offset = py * surface->pitch + px * 4;
                    uint32_t* pixel_ptr = (uint32_t*)((uint8_t*)surface->pixels + pixel_offset);
                    uint32_t existing = *pixel_ptr;

                    if (surface->format == PIXEL_FORMAT_BGRA_8888) {
                        bg_b = (existing >> 0) & 0xFF;
                        bg_g = (existing >> 8) & 0xFF;
                        bg_r = (existing >> 16) & 0xFF;
                    } else {
                        bg_r = (existing >> 0) & 0xFF;
                        bg_g = (existing >> 8) & 0xFF;
                        bg_b = (existing >> 16) & 0xFF;
                    }

                    // Alpha blend: result = fg * alpha + bg * (255 - alpha)
                    uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                    uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                    uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

                    if (surface->format == PIXEL_FORMAT_BGRA_8888) {
                        *pixel_ptr = (0xFF << 24) | (out_r << 16) | (out_g << 8) | out_b;
                    } else {
                        *pixel_ptr = (0xFF << 24) | (out_b << 16) | (out_g << 8) | out_r;
                    }
                    break;
                }

                case PIXEL_FORMAT_RGB_888: {
                    pixel_offset = py * surface->pitch + px * 3;
                    uint8_t* pixel_ptr = (uint8_t*)surface->pixels + pixel_offset;
                    bg_r = pixel_ptr[0];
                    bg_g = pixel_ptr[1];
                    bg_b = pixel_ptr[2];

                    pixel_ptr[0] = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                    pixel_ptr[1] = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                    pixel_ptr[2] = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
                    break;
                }

                case PIXEL_FORMAT_BGR_888: {
                    pixel_offset = py * surface->pitch + px * 3;
                    uint8_t* pixel_ptr = (uint8_t*)surface->pixels + pixel_offset;
                    bg_b = pixel_ptr[0];
                    bg_g = pixel_ptr[1];
                    bg_r = pixel_ptr[2];

                    pixel_ptr[0] = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
                    pixel_ptr[1] = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                    pixel_ptr[2] = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                    break;
                }

                case PIXEL_FORMAT_RGB_565: {
                    pixel_offset = py * surface->pitch + px * 2;
                    uint16_t* pixel_ptr = (uint16_t*)((uint8_t*)surface->pixels + pixel_offset);
                    uint16_t existing = *pixel_ptr;

                    // Extract 5-6-5 components and scale to 8-bit
                    bg_r = ((existing >> 11) & 0x1F) << 3;
                    bg_g = ((existing >> 5) & 0x3F) << 2;
                    bg_b = (existing & 0x1F) << 3;

                    uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
                    uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
                    uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

                    *pixel_ptr = ((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3);
                    break;
                }

                default:
                    // For unsupported formats, just write if alpha > 128
                    if (alpha > 128) {
                        uint32_t fg_pixel = graphics_color_to_pixel(style->foreground, surface->format);
                        if (surface->format == PIXEL_FORMAT_RGB_555) {
                            pixel_offset = py * surface->pitch + px * 2;
                            uint16_t* pixel_ptr = (uint16_t*)((uint8_t*)surface->pixels + pixel_offset);
                            *pixel_ptr = (uint16_t)fg_pixel;
                        }
                    }
                    break;
            }
        }
    }

    return GRAPHICS_SUCCESS;
}

// Attempt to load system font from sys.conf (tries /usr/share/sysconf and /etc)
static graphics_result_t load_system_font_from_config(void) {
    static const char* config_paths[] = {
        "/usr/share/sysconf/sys.conf",
        "/etc/sys.conf"
    };

    const uint8_t* data = NULL;
    uint32_t size = 0;
    const char* config_path = NULL;

    // Read config file from any known location
    for (size_t i = 0; i < (sizeof(config_paths) / sizeof(config_paths[0])); i++) {
        data = NULL;
        size = 0;
        if (vfs_read_file(config_paths[i], &data, &size)) {
            config_path = config_paths[i];
            break;
        }
    }

    if (!config_path) {
        debuglog(DEBUG_INFO, "System config not found at /usr/share/sysconf/sys.conf or /etc/sys.conf\n");
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    if (!data || size == 0) {
        return GRAPHICS_ERROR_INVALID_PARAMETER;
    }

    // Parse config file looking for "font=" line
    char font_path[256] = {0};
    uint8_t font_size = 16;  // Default size

    const char* p = (const char*)data;
    const char* end = p + size;

    while (p < end) {
        // Skip whitespace and empty lines
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        if (p >= end) break;

        // Skip comment lines
        if (*p == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }

        // Look for "font=" or "fontsize="
        if (strncmp(p, "font=", 5) == 0) {
            p += 5;
            // Extract path until end of line
            size_t i = 0;
            while (p < end && *p != '\n' && *p != '\r' && i < sizeof(font_path) - 1) {
                font_path[i++] = *p++;
            }
            font_path[i] = '\0';
            // Trim trailing whitespace
            while (i > 0 && (font_path[i-1] == ' ' || font_path[i-1] == '\t')) {
                font_path[--i] = '\0';
            }
        } else if (strncmp(p, "fontsize=", 9) == 0) {
            p += 9;
            // Parse font size
            int val = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (val > 0 && val <= 72) {
                font_size = (uint8_t)val;
            }
        }

        // Skip to next line
        while (p < end && *p != '\n') p++;
    }

    // If no font path found, return error
    if (font_path[0] == '\0') {
        debuglog(DEBUG_INFO, "No 'font=' entry in config file\n");
        return GRAPHICS_ERROR_NOT_SUPPORTED;
    }

    debuglog(DEBUG_INFO, "Loading system font from config (%s): '%s' at %u pt\n",
             config_path, font_path, font_size);

    // Load the font
    font_t* loaded_font = NULL;
    graphics_result_t result = font_load_from_file(font_path, "system", font_size, &loaded_font);

    if (result == GRAPHICS_SUCCESS && loaded_font) {
        // Set as system font
        font_renderer_state.system_font = loaded_font;
        font_renderer_state.font_count++;
        return GRAPHICS_SUCCESS;
    }

    debuglog(DEBUG_WARN, "Failed to load font '%s', using fallback\n", font_path);
    return result;
}

// Load the system font if the initrd is available and we haven't tried yet.
static void maybe_load_system_font_from_config(void) {
    if (!font_renderer_state.pending_config_load) {
        return;
    }

    if (ramdisk_file_count() == 0) {
        return; // initrd not ready yet
    }

    font_renderer_state.pending_config_load = false;

    graphics_result_t config_result = load_system_font_from_config();
    if (config_result == GRAPHICS_SUCCESS) {
        debuglog(DEBUG_INFO, "Loaded TrueType system font from config\n");
    } else {
        // Try to load a default TTF font as system font
        font_t* default_font = NULL;
        graphics_result_t ttf_result = font_load_from_file("/usr/share/fonts/trim.ttf", "system", 12, &default_font);
        if (ttf_result == GRAPHICS_SUCCESS && default_font) {
            font_renderer_state.system_font = default_font;
            debuglog(DEBUG_INFO, "Loaded trim.ttf as default system font\n");
        } else {
            debuglog(DEBUG_INFO, "Using built-in 8x8 font as system font\n");
        }
    }
}

// Helper to get or rasterize a glyph for bitmap fonts
static font_glyph_t* get_or_rasterize_glyph(font_t* font, uint32_t codepoint, uint32_t* out_index) {
    if (!font || !font->glyphs) {
        if (out_index) *out_index = 0;
        return NULL;
    }

    uint32_t idx = find_glyph_index(font, codepoint);
    if (out_index) *out_index = idx;

    if (idx < font->num_glyphs) {
        return &font->glyphs[idx];
    }

    return NULL;
}
