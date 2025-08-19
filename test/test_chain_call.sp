class A {
    new() {}

    method() -> i32 {
        return 10;
    }
}

class B {
    let a: A;

    new() {
        a = A.new();
    }

    a -> A {
        return a;
    }
}

System.print(B.new().a.method());
