class A {
    getter setter let num: i32;

    new(n: i32) {
        num = n;
    }
}

let a = A.new(10);
a.num += 2;
if a.num != 12 {
    Thread.abort("+= error: a.num = %(a.num)");
}

a.num /= 2;
if a.num != 6 {
    Thread.abort("/= error: a.num = %(a.num)");
}

a.num *= 2;
if a.num != 12 {
    Thread.abort("*= error: a.num = %(a.num)");
}

a.num -= 2;
if a.num != 10 {
    Thread.abort("-= error: a.num = %(a.num)");
}

a.num %= 2;
if a.num != 0 {
    Thread.abort("\%= error: a.num = %(a.num)");
}

a.num = 0u32;

a.num &= 1;
if a.num != 0 {
    Thread.abort("&= error: a.num = %(a.num)");
}

a.num |= 1;
if a.num != 1 {
    Thread.abort("|= error: a.num = %(a.num)");
}

a.num ^= 1;
if a.num != 0 {
    Thread.abort("^= error: a.num = %(a.num)");
}

let b = 10;
b += 2;
if b != 12 {
    Thread.abort("+= error: b = %(b)");
}

b /= 2;
if b != 6 {
    Thread.abort("/= error: b = %(b)");
}

b *= 2;
if b!= 12 {
    Thread.abort("*= error: b = %(b)");
}

b -= 2;
if b!= 10 {
    Thread.abort("-= error: b = %(b)");
}

b %= 2;
if b != 0 {
    Thread.abort("\%= error: b = %(b)");
}

b = 0u32;

b &= 1;
if b != 0 {
    Thread.abort("&= error: b = %(b)");
}

b |= 1;
if b != 1 {
    Thread.abort("|= error: b = %(b)");
}

b ^= 1;
if b != 0 {
    Thread.abort("^= error: b = %(b)");
}

