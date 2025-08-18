#ifndef __DIS_ASM_DISASSEMBLE_H__
#define __DIS_ASM_DISASSEMBLE_H__

#include "obj_fn.h"

void dis_asm(VM* vm, ObjModule* module, ObjFn* chunk);
void print_value(Value* val);

#endif