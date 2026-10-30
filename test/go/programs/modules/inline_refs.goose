// A module of inline_refs.goose and inline_refs_debug.goose (issue #24):
// by-reference parameters of inlined functions. Where a body is inlined, a
// parameter bound to a place of a Go local's own -- a local, a parameter, a
// field of one, an element of a fixed array of one -- names that place, which
// the body reads and writes in place, and the local stays a Go variable
// unless its address outlives the body. Each form here runs as the C backend
// runs it: scalars, fields, elements and whole structs and arrays by
// reference, a reference handed on to another inlined body, two references to
// one place, an index that changes after its element was bound, a by-value
// parameter by const reference; and the references that do outlive their
// body, returned, kept, handed to a call, compared for identity, sliced and
// printed; loops, breaks, continues, early returns and defers in a body; a
// match on an ADT by reference; a packed struct, a slice, a limited array,
// bools and floats by reference; and a self-call's base case, inlined at it
// with its arguments.
import std;

struct Pt { x: i64, y: i64 }
struct Packed { a: u8, b: i64 }
enum Shape { Dot, Circle { r: i64 }, Rect { w: i64, h: i64 } }

inline fn bump(x: i64&) { x += 1; }

inline fn bump2(x: i64&) {
    bump(x);
    x = x * 2 + 1;
}

inline fn tripled(x: const i64&) -> i64 { x * 3 }

inline fn both(x: i64&, y: i64&) {
    x += 1;
    y += 10;
    x += y;
}

inline fn shift(x: i64&, k: i64&) {
    k += 1;
    x += k;
}

inline fn nudge(p: Pt&) {
    p.x += 3;
    bump(p.y);
    p = Pt { x: p.y, y: p.x };
}

inline fn swap_xy(p: Pt&) {
    let t = p.x;
    p.x = p.y;
    p.y = t;
}

inline fn mark(a: u8[8]&, i: i64) {
    a[i] += 1;
    a[0] = a[i] + 1;
}

inline fn total(a: const i64[4]&) -> i64 {
    var s = 0;
    for x in a { s += x; }
    s + a.len
}

inline fn middle(a: i64[4]&) -> i64 {
    let s = a[1..3];
    s[0] + s[1]
}

inline fn larger(a: i64&, b: i64&) -> i64& { if a > b { a } else { b } }

inline fn same(a: i64&, b: i64&) -> bool { a .== b }

// Goose inlines no function of several results.
fn keep(x: i64&) -> i64, bool {
    x += 1;
    return x, true;
}

inline fn kept(x: i64&) -> i64 {
    let k, ok = keep(x);
    if ok { k } else { 0 }
}

inline fn find(a: i64[8]&, v: i64) -> i64 {
    for x, i in a {
        if x == v { return i; }
    }
    -1
}

inline fn count_to(x: i64&, n: i64) {
    while true {
        if x >= n { break; }
        x += 1;
        if x % 3 == 0 { continue; }
        x += 1;
    }
}

inline fn deferred(x: i64&) -> i64 {
    defer { x *= 10; }
    x += 1;
    x
}

inline fn area(s: const Shape&) -> i64 {
    match s {
        Dot => 0,
        Circle c => c.r * c.r * 3,
        Rect r => r.w * r.h,
    }
}

inline fn reshape(s: Shape&, k: i64) {
    s = if k > 0 { Shape.Circle { r: k } } else { Shape.Dot };
}

inline fn set_packed(p: Packed&) {
    p.b += 5;
    p.a = 7;
}

inline fn show(x: i64&) { print("show ", x, " ", x + 1); }

inline fn advance(s: u8[:]&) {
    if s.len > 0 { s = s[1..]; }
}

inline fn fill(a: i64[..4]&, v: i64) {
    while a.len < 4 { a.push(v + a.len); }
}

inline fn flip(b: bool&, f: f64&) {
    b = !b;
    f = f * 2.5 + (if b { 1.0 } else { 0.0 });
}

fn param_ref(n: i64) -> i64 { tripled(n) * 2 }

// A self-call's base case is inlined at it, its arguments bound in front of
// it; the call in the other arm hands the reference on.
recursive fn depth(x: i64&, n: i64) -> i64 {
    if n <= 0 { return x + 100; }
    var y = x + 1;
    depth(y, n - 1)
}

fn inline_refs_run() {
    var c = 0;
    for i in 5 { bump2(c); }
    print("bump2 ", c, " ", tripled(c), " ", param_ref(4));

    var d = 1;
    both(d, d);
    print("both ", d);

    var e: i64[4] = [1, 2, 3, 4];
    var k = 1;
    shift(e[k], k);
    print("shift ", e, " ", k);

    var p = Pt { x: 1, y: 2 };
    nudge(p);
    bump(p.x);
    swap_xy(p);
    print("nudge ", p.x, " ", p.y);

    var a: u8[8] = [0, 0, 0, 0, 0, 0, 0, 0];
    for i in 9 { mark(a, i % 8); }
    print("mark ", a[0], " ", a[3], " ", a[7]);
    print("total ", total(e), " middle ", middle(e));

    var m = 3;
    var n = 7;
    let r: i64& = larger(m, n);
    r += 100;
    print("larger ", m, " ", n, " same ", same(m, n), " ", same(m, m));

    var q = 41;
    print("kept ", kept(q), " ", q);

    var f: i64[8] = [5, 6, 7, 8, 9, 10, 11, 12];
    print("find ", find(f, 9), " ", find(f, 99));

    var w = 0;
    count_to(w, 10);
    print("count_to ", w);

    var z = 4;
    let dz = deferred(z);
    print("deferred ", dz, " ", z);

    var s: Shape = Shape.Rect { w: 3, h: 4 };
    var o: Shape = Shape.Circle { r: 2 };
    print("area ", area(s), " ", area(o));
    reshape(s, 5);
    reshape(o, 0);
    print("reshape ", area(s), " ", area(o));

    var pk = Packed { a: 1, b: 2 };
    set_packed(pk);
    print("packed ", pk.a, " ", pk.b);

    var v = 9;
    show(v);

    var bytes: u8[4] = [10, 20, 30, 40];
    var view: u8[:] = bytes;
    advance(view);
    advance(view);
    print("advance ", view.len, " ", view[0]);

    var lim: i64[..4] = [7];
    fill(lim, 3);
    print("fill ", lim);

    var b = false;
    var fl = 1.0;
    flip(b, fl);
    flip(b, fl);
    print("flip ", b, " ", fl);

    var x = 0;
    print("depth ", depth(x, 3), " ", x);
}
