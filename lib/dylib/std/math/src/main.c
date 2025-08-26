#include "libsprmath.h"
#include "sparrow.h"
#include "math.h"

static SprApi api;

static bool value_to_f64(Value* val, f64* res, const char* msg) {
    switch (val->type) {
        case VT_F64:
            *res = val->f64val;
            return true;
        case VT_U32:
            *res = val->u32val;
            return true;
        case VT_I32:
            *res = val->i32val;
            return true;
        case VT_U8:
            *res = val->u8val;
            return true;
        default:
            api.set_error(&api, msg);
            return false;
    }
}
#define value2_to_f64(val1, res1, val2, res2, msg) (value_to_f64(val1, res1, msg) && value_to_f64(val2, res2, msg))

// fn(f64) -> f64 的原生函数实现
#define f64_Fn_f64(func) \
    static bool prim_Math_##func(VM* vm, Value* args) { \
        switch (args[1].type) { \
            case VT_I32: \
                args[0] = (Value) {.type = VT_F64, .f64val = func((f64)args[1].i32val)}; \
                return true; \
            case VT_U32: \
                args[0] = (Value) {.type = VT_F64, .f64val = func((f64)args[1].u32val)}; \
                return true; \
            case VT_U8: \
                args[0] = (Value) {.type = VT_F64, .f64val = func((f64)args[1].u8val)}; \
                return true; \
            case VT_F64: \
                args[0] = (Value) {.type = VT_F64, .f64val = func(args[1].f64val)}; \
                return true; \
            default: \
                api.set_error(&api, "Math." #func "(Number) -> f64;"); \
                return false; \
        } \
    }

static bool prim_Math_pi(VM* vm, Value* args) {
    args[0] = (Value) {.type = VT_F64, .f64val = 3.141592653589793};
    return true;
}

#define Trigonometric(func) f64_Fn_f64(func)
Trigonometric(sin)
Trigonometric(cos)
Trigonometric(tan)
Trigonometric(asin)
Trigonometric(acos)
Trigonometric(atan)

static bool prim_Math_atan2(VM* vm, Value* args) {
    f64 a, b;

    if (!value2_to_f64(&args[1], &a, &args[2], &b, "Math.atan2(Number, Number) -> f64;")) {
        return false;
    }

    args[0] = (Value) {.type = VT_F64, .f64val = atan2(a, b)};
    return true;
}

static bool prim_Math_xor(VM* vm, Value* args) {
    if (args[1].type != VT_U32 || args[2].type != VT_U32) {
        api.set_error(&api, "Math.xor(u32, u32) -> u32");
        return false;
    }
    args[0] = (Value) {.type = VT_U32, .u32val = (args[1].u32val ^ args[2].u32val)};
    return true;
}

static bool prim_Math_is_nan(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_F64:
            args[0] = (Value) {.type = isnan(args[1].f64val) ? VT_TRUE : VT_FALSE};
            return true;
        default:
            args[0] = (Value) {.type = VT_FALSE};
            return true;
    }
}

static bool prim_Math_is_inf(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_F64:
            args[0] = (Value) {.type = isinf(args[1].f64val) ? VT_TRUE : VT_FALSE};
            return true;
        default:
            args[0] = (Value) {.type = VT_FALSE};
            return true;
    }
}

static bool prim_Math_abs(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
        case VT_U32:
            args[0] = args[1];
            return true;
        case VT_I32: {
            i32 val = args[0].i32val;
            args[0] = (Value) {.type = VT_I32, .i32val = val < 0 ? -val : val};
            return true;
        }
        case VT_F64: {
            f64 val = args[0].f64val;
            args[0] = (Value) {.type = VT_F64, .f64val = val < 0 ? -val : val};
            return true;
        }
        default:
            api.set_error(&api, "Math.abs(Number) -> Number");
            return false;
    }
}

static bool prim_Math_ceil(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].i32val};
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_F64, .f64val = ceil(args[0].f64val)};
            return true;
        default:
            api.set_error(&api, "Math.ceil(Number) -> f64");
            return false;
    }
}

static bool prim_Math_floor(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].i32val};
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_F64, .f64val = floor(args[0].f64val)};
            return true;
        default:
            api.set_error(&api, "Math.floor(Number) -> f64");
            return false;
    }
}

static bool prim_Math_fraction(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
        case VT_U32:
        case VT_I32:
            args[0] = (Value) {.type = VT_F64, .f64val = 0.0};
            return true;
        case VT_F64: {
            double dummy;
            args[0] = (Value) {.type = VT_F64, .f64val = modf(args[0].f64val, &dummy)};
            return true;
        }
        default:
            api.set_error(&api, "Math.fraction(Number) -> f64");
            return false;
    }
}

static bool prim_Math_truncate(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_I32, .i32val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_I32, .i32val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = args[1];
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_I32, .i32val = trunc(args[1].f64val)};
            return true;
        default:
            api.set_error(&api, "Math.truncate(Number) -> i32");
            return false;
    }
}

static bool prim_Math_i32(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_I32, .i32val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_I32, .i32val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = args[1];
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_I32, .i32val = args[1].f64val};
            return true;
        default:
            api.set_error(&api, "Math.i32(Number) -> i32");
            return false;
    }
}

static bool prim_Math_u32(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_U32, .u32val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = args[1];
            return true;
        case VT_I32:
            args[0] = (Value) {.type = VT_U32, .u32val = args[1].i32val};
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_U32, .u32val = args[1].f64val};
            return true;
        default:
            api.set_error(&api, "Math.u32(Number) -> u32");
            return false;
    }
}

static bool prim_Math_f64(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u8val};
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = (Value) {.type = VT_F64, .f64val = args[1].i32val};
            return true;
        case VT_F64:
            args[0] = args[1];
            return true;
        default:
            api.set_error(&api, "Math.f64(Number) -> f64");
            return false;
    }
}

static bool prim_Math_u8(VM* vm, Value* args) {
    switch (args[1].type) {
        case VT_U8:
            args[0] = args[1];
            return true;
        case VT_U32:
            args[0] = (Value) {.type = VT_U8, .u8val = args[1].u32val};
            return true;
        case VT_I32:
            args[0] = (Value) {.type = VT_U8, .u8val = args[1].i32val};
            return true;
        case VT_F64:
            args[0] = (Value) {.type = VT_U8, .u8val = args[1].f64val};
            return true;
        default:
            api.set_error(&api, "Math.u8(Number) -> u8");
            return false;
    }
}

f64_Fn_f64(sqrt)

void pub_spr_dylib_init(SprApi api_in) {
    api = api_in;
    
    // 数学常数
    api.register_method(&api, "pi", prim_Math_pi, true);

    // 三角函数
    api.register_method(&api, "sin(_)", prim_Math_sin, true);
    api.register_method(&api, "cos(_)", prim_Math_cos, true);
    api.register_method(&api, "tan(_)", prim_Math_tan, true);
    api.register_method(&api, "asin(_)", prim_Math_asin, true);
    api.register_method(&api, "acos(_)", prim_Math_acos, true);
    api.register_method(&api, "atan(_)", prim_Math_atan, true);
    api.register_method(&api, "atan2(_,_)", prim_Math_atan2, true);

    // 未绑定运算符的运算
    api.register_method(&api, "xor(_,_)", prim_Math_xor, true);
    api.register_method(&api, "sqrt(_)", prim_Math_sqrt, true);
    api.register_method(&api, "is_nan(_)", prim_Math_is_nan, true);
    api.register_method(&api, "is_inf(_)", prim_Math_is_inf, true);
    api.register_method(&api, "abs(_)", prim_Math_abs, true);
    api.register_method(&api, "ceil(_)", prim_Math_ceil, true);
    api.register_method(&api, "floor(_)", prim_Math_floor, true);
    api.register_method(&api, "fraction(_)", prim_Math_fraction, true);
    api.register_method(&api, "truncate(_)", prim_Math_truncate, true);

    // 数值类型转换
    api.register_method(&api, "i32(_)", prim_Math_i32, true);
    api.register_method(&api, "u32(_)", prim_Math_u32, true);
    api.register_method(&api, "u8(_)", prim_Math_u8, true);
    api.register_method(&api, "f64(_)", prim_Math_f64, true);
}
