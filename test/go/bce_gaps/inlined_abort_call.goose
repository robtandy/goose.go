// Gap 1 of issue #26, a second form: an inlined body that cannot complete
// is taken to complete. `fail` only aborts, so past `if bad { fail(...); }`
// the index is in range, as it is past `if bad { abort(...); }`; but once
// fail is inlined, InlineBlock::BceWalk (bce.h) reports that its body falls
// through whatever the body's own walk found, and the guard's then-branch
// joins the code after it with the facts of a bad index. (Where fail is not
// inlined, -O0, BCE takes no call to end the path.)

fn fail(msg: const u8[:]) { abort(msg); }

fn at(xs: const i64[:], di: i64) -> i64 {
    if di < 0 || di >= xs.len { fail("index out of range"); }
    xs[di] // bce:keep
}

fn at_abort(xs: const i64[:], di: i64) -> i64 {
    if di < 0 || di >= xs.len { abort("index out of range"); }
    xs[di] // bce:elide
}

// The indices come from data, of no sign BCE knows at the calls.
fn main() {
    let xs = [5, 6, 7];
    var s = 0;
    for x in xs { s += at(xs, x - 5) + at_abort(xs, x - 5); }
    print(s);
}
