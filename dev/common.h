#ifndef __INCLUDE_COMMON_H__
#define __INCLUDE_COMMON_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef double f64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef int64_t i64;
typedef size_t usize;

typedef struct _VM VM;
typedef struct _Parser Parser;
typedef struct _Class Class;

#ifndef __STDBOOL_H
    #include <stdbool.h>
#endif

#define UNUSED __attribute__ ((unused))

#ifdef DEBUG_ASSERT_ON
    #define ASSERT(condition, err_msg) \
        do {\
            if (!(condition)) {\
                fprintf(\
                    stderr, "%s:%d: ASSERT failed in function '%s'. message: %s\n",\
                    __FILE__, __LINE__, __func__, err_msg\
                );\
                abort();\
            }\
        } while(0)

    #define UNREACHABLE() \
        do {\
            fprintf(stderr, "%s:%d: unreachbale. in function '%s'", __FILE__, __LINE__, __func__);\
            abort();\
        } while(0)
#else
    #define ASSERT(condition, err_msg) ((void)0)
    #define UNREACHABLE() ((void)0)
#endif

#endif