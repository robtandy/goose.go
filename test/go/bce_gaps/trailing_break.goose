// Gap 2 of issue #26: a loop bounded by the break that ends its body, not by
// its condition, keeps every check its bound would remove. i < xs.len holds
// at the top of every iteration of `sum_until` -- the guard before the loop
// states it for the first, and the break leaves before any other where it
// would not hold -- but BCE carries no fact around a loop: While::BceWalk
// (bce.h) strips every fact on what the body writes at the loop's entry,
// and at the top of the body only the condition's facts hold. `copy_some`
// is a kernel's shape: the condition bounds the source's index, which is
// elided, and the trailing break the destination's, which is not; with
// both bounds in the condition (`copy_both`) both are elided.

fn sum_until(xs: const i64[:], t: i64) -> i64 {
    if xs.len == 0 { return 0; }
    var i = 0;
    var s = 0;
    while s <= t {
        s += xs[i]; // bce:keep
        i += 1;
        if i >= xs.len { break; }
    }
    s
}

fn copy_some(xs: const i64[:], o: i64[:]) -> i64 {
    if o.len == 0 { return 0; }
    var si = 0;
    var di = 0;
    while si < xs.len {
        let x = xs[si]; // bce:elide
        o[di] = x; // bce:keep
        si += 1;
        di += 1;
        if di >= o.len { break; }
    }
    di
}

fn copy_both(xs: const i64[:], o: i64[:]) -> i64 {
    var si = 0;
    var di = 0;
    while si < xs.len && di < o.len {
        let x = xs[si]; // bce:elide
        o[di] = x; // bce:elide
        si += 1;
        di += 1;
    }
    di
}

fn main() {
    let xs = [1, 2, 3, 4, 5];
    var o = [0, 0, 0];
    print(sum_until(xs, 4), " ", copy_some(xs, o), " ", copy_both(xs, o), " ", o[2]);
}
