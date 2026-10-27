// print (§3.7): any number of arguments, nothing inserted between them, a
// newline after, one write per line; the text of every scalar; a string
// literal's bytes as they are, escapes and UTF-8 included; and a line built
// before an argument that prints on its own writes anything.

fn shout(n: i64) -> i64 {
    print("shout ", n);
    n * 2
}

recursive fn countdown(n: i64) {
    if n == 0 {
        print("liftoff");
        return;
    }
    print("t-", n);
    countdown(n - 1);
}

fn main() {
    print();
    print("");
    print("hello, world");
    print("tab\tquote\" backslash\\ bell\x07 nul-free");
    print("multi\nline\nstring");
    print("utf-8: héllo wörld ✓ 🦆");
    print("a", "b", "c", 1, 2, 3, true, false, 1.5, -0.25);
    print("i8 ", -128 as i8, " u8 ", 255 as u8, " i16 ", -32768 as i16, " u16 ", 65535 as u16);
    print("i32 ", -2147483648 as i32, " u32 ", 4294967295 as u32);
    print("i64 ", -9223372036854775807 - 1, " u64 ", 18446744073709551615);
    print("f32 ", 0.1 as f32, " ", 1.0 as f32 / 3.0, " f64 ", 0.1, " ", 1.0 / 3.0);
    // The argument's own line comes first: this line is written whole after.
    print("outer ", shout(21), " end");
    print(shout(1), shout(2), shout(3));
    countdown(3);
    var line = 0;
    while line < 3 {
        print("line ", line, ": ", line * line, " ", line % 2 == 0);
        line += 1;
    }
    print("a very long line: ", 1111111111, 2222222222, 3333333333, 4444444444, 5555555555,
          6666666666, 7777777777, 8888888888, 9999999999, " and floats ", 1e100, " ", 1e-100,
          " ", 123.456, " ", -987.654321, " the end");
}
