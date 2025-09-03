#ifndef __INCLUDE_UTF8_H__
#define __INCLUDE_UTF8_H__
#include "common.h"

typedef int Char_utf8;

u32 get_byte_of_utf8_char(Char_utf8 val);
u32 get_byte_of_decode_utf8(int val);
u32 get_byte_of_decode_utf8_from_start(u8 val);
u8 encode_utf8(u8* buf, Char_utf8 val);
Char_utf8 decode_utf8(const u8* byte, u32 len);

#endif