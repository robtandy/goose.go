// A loop's binders whose address the body takes (PR #10's review): a
// reference to a count's, a range's or an array walk's binder, held in a
// packed struct, still reaches it after a deep recursion has grown Go's
// stack, which moves whatever lives on it; the binders live in the frame.
struct Held { r: const i64& }
struct Pair { a: u8, r: const i64& }

// `n - ...` has no accumulator form (optimize_tre.h): every level is a call.
recursive fn grow(n: i64) -> i64 {
    if n == 0 { return 0; }
    return n - grow(n - 1);
}

fn main() {
    var total = 0;
    for i in 3 {
        let held = Held { r: i };
        total += grow(20000);
        assert(held.r .== i);
        print("count ", i, " ", held.r);
    }
    for i in 5..8 {
        let p = Pair { a: 1, r: i };
        total += grow(20000 + i);
        assert(p.r .== i);
        print("range ", i, " ", p.r, " ", p.a);
    }
    let xs = [10, 20, 30];
    for x, i in xs {
        let held = Held { r: i };
        let p = Pair { a: 2, r: x };
        total += grow(20000);
        assert(held.r .== i);
        assert(p.r .== x);
        print("array ", i, " ", held.r, " ", p.r);
    }
    for x, i in 2..4 {
        let held = Held { r: i };
        total += grow(20000);
        assert(held.r .== i);
        print("indexed range ", x, " ", i, " ", held.r);
    }
    print(total);
}
