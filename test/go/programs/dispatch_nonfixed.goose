// Case functions with results that are not fixed-size (PR #10's review):
// each case builds an array at the caller's destination, beside a fixed
// result, received by a binding of both, forwarded by a return, used as a
// value and read as a slot of another kind.
enum E { A { n: i64 }, B { lo: i64, hi: i64 }, C }

fn pair(a: E.A) -> i64[>..], i64 {
    var xs: i64[>..] = [];
    for i in a.n { xs.push(i * i); }
    return xs, a.n;
}
fn pair(b: E.B) -> i64[>..], i64 {
    var xs: i64[>..] = [];
    for i in b.lo..b.hi { xs.push(i); }
    return xs, b.hi - b.lo;
}
fn pair(c: E.C) -> i64[>..], i64 {
    var xs: i64[>..] = [];
    return xs, -1;
}

fn forward(e: E) -> i64[>..], i64 { return pair(e); }

recursive fn deep(e: E, d: i64) -> i64[>..], i64 {
    if d > 0 { return deep(e, d - 1); }
    return pair(e);
}

fn words(a: E.A) -> u8[>..] { str("a", a.n) }
fn words(b: E.B) -> u8[>..] { str("b", b.lo, "-", b.hi) }
fn words(c: E.C) -> u8[>..] { str("c") }

fn first(s: const u8[:]) -> u8 { s[0] }

fn sum(xs: const i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    s
}

fn main() {
    let es: E[3] = [E.A { n: 4 }, E.B { lo: 3, hi: 7 }, E.C];
    for e in es {
        let xs, n = pair(e);
        print(n, " ", xs.len, " ", xs[..]);
        var ys, m = forward(e);
        ys.push(100);
        print(m, " ", ys.len, " ", sum(ys));
        let zs, k = deep(e, 2);
        print(k, " ", sum(zs));
        let w = words(e);
        print(w, " ", w.len);
        print(first(words(e)), " ", sum(pair(e)));
    }
}
