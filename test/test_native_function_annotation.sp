class Foo {
    native static bar -> i31;
    native static a(b: String) -> List<f64>;
    native static foobar();

    native + (other: Foo) -> Foo;
    native - -> Foo;

    native b(c: String);
    native d -> i32;
    native e = (val: Foo);
    native f();
    
    native [from: i32, to: f64] -> Foo;
    native [index: i32] = (val: String);
}
