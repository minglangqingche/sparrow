#ifndef __VM_OPCODE_H__
#define __VM_OPCODE_H__

#define OPCODE_SLOTS(opcode, effect) OPCODE_##opcode,
typedef enum {
    #include "opcode.inc"
} OpCode;
#undef OPCODE_SLOTS

#endif