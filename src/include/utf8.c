#include "utf8.h"
#include "common.h"

u32 get_byte_of_utf8_char(Char_utf8 val) {
    ASSERT(val > 0, "Can't encode nagative value.");

    if (val <= 0x7F) {
        return 1;
    }

    if (val <= 0x7FF) {
        return 2;
    }

    if (val <= 0x10FFFF) {
        return 4;
    }

    return 0;
}

u32 get_byte_of_decode_utf8(u8 val) {
    if ((val & 0xC0) == 0x80) {
        return 0;
    }

    if ((val & 0xF8) == 0xF0) {
        return 4;
    }

    if ((val & 0xF0) == 0xE0) {
        return 3;
    }

    if ((val & 0xE0) == 0xC0) {
        return 2;
    }

    return 1;
}

u8 encode_utf8(u8* buf, Char_utf8 val) {
    ASSERT(val > 0, "Can't encode nagative value.");

    if (val <= 0x7F) {
        *buf = val & 0x7F;
        return 1;
    } else if (val <= 0x7FF) {
        *buf++ = 0xC0 | ((val & 0x7C0) >> 6);
        *buf = 0x80 | (val & 0x3f);
        return 2;
    } else if (val <= 0xFFFF) {
        *buf++ = 0xE0 | ((val & 0xf000) >> 12);
        *buf++ = 0x80 | ((val & 0xfC0) >> 6);
        *buf = 0x80 | (val & 0x3F);
        return 3;
    } else if (val <= 0x10FFFF) {
        *buf++ = 0xF0 | ((val & 0x1C0000) >> 18);
        *buf++ = 0x80 | ((val & 0x3F000) >> 12);
        *buf++ = 0x80 | ((val & 0xFC0) >> 6);
        *buf++ = 0x80 | (val & 0x3F);
        return 4;
    }

    UNREACHABLE();
    return 0;
}

Char_utf8 decode_utf8(const u8* byte, u32 len) {
    if (*byte <= 0x7F) {
        return *byte;
    }

    int val = 0;
    u32 remaining = 0;

    if ((*byte & 0xE0) == 0xC0) {
        val = *byte & 0x1F;
        remaining = 1;
    } else if ((*byte & 0xF0) == 0xE0) {
        val = *byte & 0x0F;
        remaining = 2;
    } else if ((*byte & 0xF8) == 0xF0) {
        val = *byte & 0x07;
        remaining = 3;
    } else {
        return -1;
    }

    if (remaining > len - 1) {
        return -1;
    }

    for (; remaining > 0; remaining--) {
        byte++;
        if ((*byte & 0xC0) != 0x80) {
            return -1;
        }

        val = val << 6 | (*byte & 0x3F);
    }
    return val;
}
