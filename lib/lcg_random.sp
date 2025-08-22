class LCGRandom {
    static let seed: f64 = System.clock(); // 默认初始种子

    static let a: f64 = 1103515245.0;    // 乘数
    static let c: f64 = 12345.0;         // 增量
    static let m: f64 = 2147483648.0;    // 模数(2^31)，确保结果在int范围内

    static rand() -> f64 {
        seed = Math.i32((a * seed + c) % m);
        return seed / m;
    }

    static rand_i32(min: i32, max: i32) -> i32 {
        if (min >= max) {
            return max; // 区间无效，默认返回max
        }

        let res: f64 = rand();
        return min + Math.i32(res * (max - min + 1.0));
    }
}
