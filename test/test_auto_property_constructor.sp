class A {
    getter let a: i32;
    getter let b: String?;
    getter let c: Map<String, Any>;

    new{a: i32, b: String?, c: Map<String, Any>}() {}
    
    new{a: i32, b: String?}() {
        c = {
            "a": a,
            "b": b,
        };
    }

    to_string() -> String {
        return "<a: %(a), b: %(b), c: %(c)>";
    }
}

let a = A.new(10, "this is b");
System.print(a);
