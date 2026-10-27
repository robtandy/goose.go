// Identity (§3.8, §3.10): distinct variables have distinct addresses, those
// of no bytes too -- a zero-length array, a payload-less variant -- and an
// empty slice is where it starts, the end of its array included, so slices
// at different places differ as members of a compared value.
enum E { Nil, One { v: i64 } }
struct S { s: i64[:] }

recursive fn same<T>(a: T&, b: T&, n: i64) -> bool {
    if n > 0 { return same(a, b, n - 1); }
    return a .== b;
}

fn main() {
    var x: i64[0] = [];
    var y: i64[0] = [];
    let p: E = E.Nil;
    let q: E = E.Nil;
    print(same(x, y, 3), " ", same(x, x, 3), " ", same(p, q, 3), " ", same(p, p, 3));

    var a: i64[2] = [1, 2];
    let s0 = S { s: a[0..0] };
    let s1 = S { s: a[1..1] };
    let s2 = S { s: a[2..2] };
    let t0 = S { s: a[0..0] };
    let t2 = S { s: a[2..2] };
    print(s0 == s2, " ", s0 == s1, " ", s1 == s2, " ", s0 == t0, " ", s2 == t2);
    let whole = a[..];
    print(whole[2..].len, " ", a[2..2] == a[0..0]);
}
