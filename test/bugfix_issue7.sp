class TestClass {
    static st_getter -> String {
        return "this is static getter method.";
    }

    static st_method() -> String {
        return "this is static method.";
    }
}

fn foo() -> String {
    return "this is function.";
}

let a = 10;

System.print("%(a)");
System.print("%(TestClass.st_getter)");
System.print("%(TestClass.st_method())"); // error: expect string at the end of interpolatation.
System.print("%(foo())"); // expect string at the end of interpolatation.
