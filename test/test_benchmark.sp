fn fib(n: u32) -> u32 {
    if n == 0u32 || n == 1u32 {
        return n;
    }
    return fib(n - 1u32) + fib(n - 2u32);
}

let n = 40u32;

let start = System.get_time();
let res = fib(n);
let end = System.get_time();

System.print("fib(%(n)) = %(res); %((end - start) / 1000)s");
