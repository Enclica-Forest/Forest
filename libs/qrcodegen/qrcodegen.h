/* 
 * QR Code generator library (Nayuki)
 * https://github.com/nayuki/QR-Code-generator
 * 
 * Copyright (c) Project Nayuki
 * SPDX-License-Identifier: MIT
 */

#ifndef QRCODEGEN_H
#define QRCODEGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define qrcodegen_VERSION_STRING "1.8.0"

#define qrcodegen_BUFFER_LEN_FOR_VERSION(version) (((version) * 4 + 17) * ((version) * 4 + 17) + 1)

#define qrcodegen_BUFFER_LEN_MAX qrcodegen_BUFFER_LEN_FOR_VERSION(40)

enum qrcodegen_Ecc {
    qrcodegen_Ecc_LOW = 0,
    qrcodegen_Ecc_MEDIUM = 1,
    qrcodegen_Ecc_QUARTILE = 2,
    qrcodegen_Ecc_HIGH = 3
};

enum qrcodegen_Mask {
    qrcodegen_Mask_AUTO = -1,
    qrcodegen_Mask_0 = 0,
    qrcodegen_Mask_1 = 1,
    qrcodegen_Mask_2 = 2,
    qrcodegen_Mask_3 = 3,
    qrcodegen_Mask_4 = 4,
    qrcodegen_Mask_5 = 5,
    qrcodegen_Mask_6 = 6,
    qrcodegen_Mask_7 = 7
};

enum qrcodegen_Mode {
    qrcodegen_Mode_NUMERIC = 0,
    qrcodegen_Mode_ALPHANUMERIC = 1,
    qrcodegen_Mode_BYTE = 2,
    qrcodegen_Mode_KANJI = 3
};

struct qrcodegen_Segment {
    enum qrcodegen_Mode mode;
    size_t numChars;
    uint8_t* data;
    size_t bitLength;
};

bool qrcodegen_generateBytes(uint8_t qrcode[], uint8_t tempBuffer[], const char* text, enum qrcodegen_Ecc ecl);

bool qrcodegen_encodeText(const char* text, uint8_t tempBuffer[], uint8_t qrcode[], enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, int mask, bool boostEcl);

bool qrcodegen_encodeSegments(const struct qrcodegen_Segment segs[], size_t len, enum qrcodegen_Ecc ecl, uint8_t qrcode[], uint8_t tempBuffer[], int minVersion, int maxVersion, int mask, bool boostEcl);

bool qrcodegen_encodeSegmentsAdvanced(const struct qrcodegen_Segment segs[], size_t len, enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, int mask, bool boostEcl, uint8_t tempBuffer[], uint8_t qrcode[]);

int qrcodegen_getSize(const uint8_t qrcode[]);

bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y);

void qrcodegen_setModule(uint8_t qrcode[], int x, int y, bool isDark);

void qrcodegen_clearScreen(uint8_t qrcode[]);

struct qrcodegen_Segment qrcodegen_makeNumeric(const char* digits, uint8_t tempBuffer[]);

struct qrcodegen_Segment qrcodegen_makeAlphanumeric(const char* text, uint8_t tempBuffer[]);

struct qrcodegen_Segment qrcodegen_makeBytes(const uint8_t* dataAndLen, size_t len, uint8_t tempBuffer[]);

struct qrcodegen_Segment qrcodegen_makeKanji(const uint8_t* dataAndLen, size_t len, uint8_t tempBuffer[]);

bool qrcodegen_isAlphanumeric(const char* text);

bool qrcodegen_isNumeric(const char* text);

bool qrcodegen_isKanji(const uint8_t* dataAndLen, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* QRCODEGEN_H */
