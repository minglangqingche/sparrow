class TestClass {
    static st_getter -> String {
        return "this is static getter method.";
    }

    static st_method() -> String {
        return "this is static method.";
    }

    let field: Any;

    new () {}

    getter -> String {
        return "this is getter method.";
    }

    setter = (_: Any) -> String {
        return "this is setter method.";
    }

    method() -> String {
        return "this is method.";
    }

    [_: Any] -> String {
        return "this is subscript method.";
    }

    [_1: Any] = (_2: Any) -> String {
        return "this is subscript setter method.";
    }

    ~ -> String {
        return "this is unary method.";
    }

    + (_: Any) -> String {
        return "this is infix method.";
    }

    - (_: Any) -> String {
        return "this is mix(infix) method.";
    }

    - -> String {
        return "this is mix(prefix) method.";
    }
}
