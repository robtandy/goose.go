// abort with a message the program computes (issue #5): the bytes of the
// slice it is handed, here a builder's, after the program's own output.
fn check(v: i64, what: const u8[:]) {
    if v < 0 {
        var m: u8[>..] = [];
        m.append(what);
        m.append(" went negative: ");
        m.append(str(v));
        abort(m[..]);
    }
}

fn main() {
    check(3, "stock");
    print("stock is 3");
    check(-2, "balance");
    print("not reached");
}
