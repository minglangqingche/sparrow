// 用于测试字符串内嵌表达式的功能是否正确

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

// 内嵌表达式前后均为空串
System.print("%(a)");
System.print("%(TestClass.st_getter)");
System.print("%(TestClass.st_method())");
System.print("%(foo())");

// 内嵌表达式前有非空字符串
System.print("prefix %(a)");
System.print("prefix %(TestClass.st_getter)");
System.print("prefix %(TestClass.st_method())");
System.print("prefix %(foo())");

// 内嵌表达式后有非空字符串
System.print("%(a) tial");
System.print("%(TestClass.st_getter) tial");
System.print("%(TestClass.st_method()) tial");
System.print("%(foo()) tial");

// 前后都包含非空字符串
System.print("prefix %(a) tial");
System.print("prefix %(TestClass.st_getter) tial");
System.print("prefix %(TestClass.st_method()) tial");
System.print("prefix %(foo()) tial");
