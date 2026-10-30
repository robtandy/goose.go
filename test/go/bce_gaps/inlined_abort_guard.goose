// Gap 1 of issue #26: a fact an inlined body states is lost after the body
// where it returns early. `checked` aborts where di < 0, so di >= 0 holds
// wherever it returns, at its early return and at its end alike, and BCE
// uses it inside the body; but `after`, where checked is inlined, cannot
// use it past the call: InlineBlock::BceWalk (bce.h) puts the flow back to
// the body's entry, less what the body writes, where a return to the body
// comes before its end.
//
// A body with no early return keeps its facts (`checked_once`): the flow
// after it is the flow at its end.

fn checked(xs: const i64[:], di: i64) -> i64 {
    if di < 0 { abort("negative index"); }
    if di >= xs.len { return 0; }
    xs[di] // bce:elide
}

fn after(xs: const i64[:], di: i64) -> i64 {
    let v = checked(xs, di);
    // di >= 0 here, and di < xs.len is tested: elidable.
    if di < xs.len { return v + xs[di]; } // bce:keep
    v
}

fn checked_once(xs: const i64[:], di: i64) -> i64 {
    if di < 0 { abort("negative index"); }
    di % 7
}

fn after_once(xs: const i64[:], di: i64) -> i64 {
    let v = checked_once(xs, di);
    if di < xs.len { return v + xs[di]; } // bce:elide
    v
}

// The indices come from data, of no sign BCE knows at the calls.
fn main() {
    let xs = [5, 6, 7];
    var s = 0;
    for x in xs { s += after(xs, x - 5) + after_once(xs, x - 4); }
    print(s);
}
