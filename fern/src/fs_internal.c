#include "include/fs_internal.h"

uint32_t fs_octal_to_uint32(const uint8_t* str, int len) {
    uint32_t n = 0;
    for (int i = 0; i < len && str[i]; i++) {
        n = (n << 3) + (str[i] - '0');
    }
    return n;
}

uint64_t fs_octal_to_uint64(const uint8_t* str, int len) {
    uint64_t n = 0;
    for (int i = 0; i < len && str[i]; i++) {
        n = (n << 3) + (str[i] - '0');
    }
    return n;
}

void fs_uint32_to_octal(uint32_t v, uint8_t* out, int len) {
    uint8_t buf[12] = {0};
    int i = 0;
    
    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v > 0 && i < 11) {
            buf[i++] = '0' + (v & 7);
            v >>= 3;
        }
    }
    
    int j = 0;
    while (j < len && i > 0) {
        out[len - 1 - j] = buf[--i];
        j++;
    }
}
