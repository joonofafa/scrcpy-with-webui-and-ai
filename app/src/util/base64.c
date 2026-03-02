#include "base64.h"

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t
sc_base64_encode(const uint8_t *data, size_t len, char *output) {
    size_t i;
    char *p = output;

    for (i = 0; i + 2 < len; i += 3) {
        *p++ = base64_table[(data[i] >> 2) & 0x3F];
        *p++ = base64_table[((data[i] & 0x3) << 4) | (data[i + 1] >> 4)];
        *p++ = base64_table[((data[i + 1] & 0xF) << 2) | (data[i + 2] >> 6)];
        *p++ = base64_table[data[i + 2] & 0x3F];
    }

    if (i < len) {
        *p++ = base64_table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            *p++ = base64_table[((data[i] & 0x3) << 4) | (data[i + 1] >> 4)];
            *p++ = base64_table[(data[i + 1] & 0xF) << 2];
        } else {
            *p++ = base64_table[(data[i] & 0x3) << 4];
            *p++ = '=';
        }
        *p++ = '=';
    }

    *p = '\0';
    return (size_t)(p - output);
}
