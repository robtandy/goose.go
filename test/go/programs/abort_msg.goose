// abort(msg) (§9.3): the message on stderr, exit status 1, and what was
// printed before it still out. A function that ends in an abort needs no
// value there.

fn checked_div(a: i64, b: i64) -> i64 {
    guard b != 0 else { abort("division by zero requested"); }
    a / b
}

fn pick(ok: bool) -> i64 {
    if ok { 42 } else { abort("no pick") }
}

fn main() {
    print(checked_div(7, 2), " ", pick(true));
    if false { abort("never"); }
    print(checked_div(1, 0));
    print("not reached");
}
