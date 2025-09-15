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

u32 get_byte_of_decode_utf8(int codepoint) {
    // 校验码点合法性
    if (codepoint < 0 || codepoint > 0x10FFFF) {
        return 0; // 超出Unicode编码范围（0x0000 ~ 0x10FFFF）
    }
    // 排除代理区码点（UTF-16专用，不允许直接在UTF-8中使用）
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        return 0;
    }
    
    // 根据码点范围判断UTF-8字节数
    if (codepoint <= 0x007F) {
        return 1; // 1字节：0x0000 ~ 0x007F
    } else if (codepoint <= 0x07FF) {
        return 2; // 2字节：0x0080 ~ 0x07FF
    } else if (codepoint <= 0xFFFF) {
        return 3; // 3字节：0x0800 ~ 0xFFFF
    } else {
        return 4; // 4字节：0x10000 ~ 0x10FFFF
    }
    return 0;
}

// 通过utf8码点起始字节判断码点长度
u32 get_byte_of_decode_utf8_from_start(u8 val) {
    // 先判断多字节起始字节（从长到短）
    if ((val & 0xF8) == 0xF0) {
        return 4; // 4字节起始字节
    }
    if ((val & 0xF0) == 0xE0) {
        return 3; // 3字节起始字节
    }
    if ((val & 0xE0) == 0xC0) {
        return 2; // 2字节起始字节
    }
    // 单字节起始字节
    if ((val & 0x80) == 0x00) {
        return 1;
    }
    // 若为后续字节（0x80~0xBF），返回错误标识（如0）
    return 0;
}

u8 encode_utf8(u8* buf, Char_utf8 val) {
    ASSERT(val >= 0, "Can't encode nagative value.");

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
