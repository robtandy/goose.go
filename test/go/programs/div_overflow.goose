// i64.min / -1 aborts in every build (§6.2), where a narrower quotient
// that does not fit wraps in a release build.

fn quotient(a: i64, b: i64) -> i64 { a / b }
fn narrow(a: i16, b: i16) -> i16 { a / b }

fn main() {
    print(narrow(-32768, -1), " ", quotient(-9223372036854775807, -1));
    print(quotient(-9223372036854775807 - 1, -1));
    print("not reached");
}
