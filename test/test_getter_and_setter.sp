class A {
    getter setter static let name = "<Class A>";
    getter setter let field;
    new() {
        field = 10;
    }
}

if A.name != "<Class A>" {
    Thread.abort("getter error: A.name = %(A.name)");
}

A.name = 10;
if A.name != 10 {
    Thread.abort("setter error: A.name = %(A.name)");
}

let a = A.new();
if a.field != 10 {
    Thread.abort("getter error: a.field = %(a.field)");
}

a.field = -999;
if a.field != -999 {
    Thread.abort("setter error: a.field = %(a.field)");
}
