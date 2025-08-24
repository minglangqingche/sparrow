class Math {
    // 数学常数
    native static pi -> f64;

    // 三角函数
    native static sin(Number<f64>) -> f64;
    native static cos(Number<f64>) -> f64;
    native static tan(Number<f64>) -> f64;
    native static asin(Number<f64>) -> f64;
    native static acos(Number<f64>) -> f64;
    native static atan(Number<f64>) -> f64;
    native static atan2(Number<f64>, Number<f64>) -> f64;

    // 未绑定运算符的运算
    native static xor(u32, u32) -> u32;
    native static sqrt(Number<f64>) -> f64;
    native static abs(T<Number>) -> T;
    native static ceil(Number<f64>) -> f64;
    native static floot(Number<f64>) -> f64;
    native static fraction(Number<f64>) -> f64;
    native static truncate(Number<f64>) -> i32;

    // 数值类型转换
    native static i32(Number) -> i32;
    native static u32(Number) -> u32;
    native static u8(Number) -> u8;
    native static f64(Number) -> f64;

    static max(a: T<Compileable>, b: T) -> T {
        return a >= b ? a : b;
    }

    static min(a: T<Compileable>, b: T) -> T {
        return a <= b ? a : b;
    }
}

let dylib_math = DyLib.c_dlopen(DyLib.SPR_DYLIB_PATH + "/std/math/build/libsprmath.dylib");
DyLib.bind(dylib_math, Math);
