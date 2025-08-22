class Random {
    // 继承该类的类只需要实现 rand() -> f64 即可

    rand_i32(min: i32, max: i32) -> i32 {
        if (min >= max) {
            return max; // 区间无效，默认返回max
        }

        let res: f64 = rand();
        return min + Math.i32(res * (max - min + 1.0));
    }
}

class LCGRandom < Random {
    static let a: f64 = 1103515245.0;    // 乘数
    static let c: f64 = 12345.0;         // 增量
    static let m: f64 = 2147483648.0;    // 模数(2^31)，确保结果在int范围内

    let seed: f64;

    new(s: f64) {
        seed = s;
    }

    new() {
        seed = Math.f64(System.clock()); // 默认初始种子
    }

    rand() -> f64 {
        seed = Math.i32((a * seed + c) % m);
        return seed / m;
    }
}

class XorshiftRandom < Random {
    static let d: f64 = 4294967296.0;

    let state: u32;

    new(seed: u32) {
        state = seed == 0 ? 1 : seed; // 如果 state == 0，生成的每一项都是 0
    }
    
    new() {
        state = System.clock();
    }

    xorshift32() -> u32 {
        let x: u32 = state;
        x = Math.xor(x, (x << 13));
        x = Math.xor(x, (x << 17));
        x = Math.xor(x, (x << 5));
        state = x;
        return x;
    }

    rand() -> f64 {
        return xorshift32() / d;
    }
}
