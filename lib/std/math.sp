class Math {
    // 数学常数
    native static pi -> f64;

    // 三角函数
    native static sin(x: Number<f64>) -> f64;
    native static cos(x: Number<f64>) -> f64;
    native static tan(x: Number<f64>) -> f64;
    native static asin(x: Number<f64>) -> f64;
    native static acos(x: Number<f64>) -> f64;
    native static atan(x: Number<f64>) -> f64;
    native static atan2(x: Number<f64>, y: Number<f64>) -> f64;

    // 未绑定运算符的运算
    native static xor(a: u32, b: u32) -> u32;
    native static sqrt(a: Number<f64>) -> f64;
    native static abs(a: T<Number>) -> T;
    native static ceil(a: Number<f64>) -> f64;
    native static floot(a: Number<f64>) -> f64;
    native static fraction(a: Number<f64>) -> f64;
    native static truncate(a: Number<f64>) -> i32;

    // 数值类型转换
    native static i32(a: Number) -> i32;
    native static u32(a: Number) -> u32;
    native static u8(a: Number) -> u8;
    native static f64(a: Number) -> f64;

    static max(a: T<Compileable>, b: T) -> T {
        return a >= b ? a : b;
    }

    static min(a: T<Compileable>, b: T) -> T {
        return a <= b ? a : b;
    }
}

let dylib_math = DyLib.c_dlopen(DyLib.SPR_DYLIB_PATH + "/std/math/build/libsprmath.dylib");
DyLib.bind(dylib_math, Math);
