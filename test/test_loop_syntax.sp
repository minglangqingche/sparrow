let i = 0;

loop {
    if i >= 10 {
        break;
    }
    i = i + 1;
}

if i != 10 {
    Thread.abort("loop error: i = %(i)");
}
