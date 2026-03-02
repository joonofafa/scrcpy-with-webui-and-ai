#ifndef SC_BASE64_H
#define SC_BASE64_H

#include <stddef.h>
#include <stdint.h>

// Returns the required buffer size for base64 encoding (including null terminator)
static inline size_t
sc_base64_encode_len(size_t input_len) {
    return ((input_len + 2) / 3) * 4 + 1;
}

// Encode data to base64. Output buffer must be at least sc_base64_encode_len(len) bytes.
// Returns the length of the encoded string (not including null terminator).
size_t
sc_base64_encode(const uint8_t *data, size_t len, char *output);

#endif
