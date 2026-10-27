// goose-flags: --debug
// A debug build checks signed arithmetic (§6.2): the operands, the
// operation and the type in the message, with the file and line. Unsigned
// arithmetic wraps in every build.

fn add(a: i32, b: i32) -> i32 { a + b }
fn mul(a: i64, b: i64) -> i64 { a * b }
fn wrap(a: u8, b: u8) -> u8 { a + b }

fn main() {
    print(add(2147483640, 7), " ", mul(3037000499, 3037000499), " ", wrap(200, 100));
    var x: i8 = 120;
    x += 7;
    print(x);
    print(add(2147483647, 1));
    print("not reached");
}
