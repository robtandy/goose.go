// Fixed-size structs (§3.2): those whose fields Go aligns where C packs
// them are native Go structs, the rest the bytes of their packed layout, with
// `pad` and `pad n` laying out the gaps. Literals, field reads and writes,
// compound assignment, copies, references, globals, equality and the text of
// print all take the layout as C has it.

struct P { x: i32, y: i32 }
struct Q { a: u8, b: i64, c: u16 }              // packed: b at byte 1
struct R { a: u8, pad, b: i32, pad 2, c: u16 }  // b aligned by `pad`
struct E {}                                     // one byte, as in C
struct Wrap { p: P, q: Q, tag: bool, f: f32 }
struct Pair<T> { first: T, second: T }
struct Defaults { id: i64, scale: f64 = 1.5, on: bool = true }
struct Floats { a: f32, b: f64 }
struct Deep { w: Wrap, pairs: Pair<i16>[2] }

fn mk(a: i32) -> P { P { x: a, y: a + 1 } }
fn sum(p: P) -> i32 { p.x + p.y }
fn bump(p: P&) { p.x += 10; p.y -= 1; }
fn swap<T>(p: Pair<T>) -> Pair<T> { Pair<T> { first: p.second, second: p.first } }
fn widen(q: Q) -> i64 { q.a + q.b + q.c }

let ORIGIN = P { x: 0, y: 0 };
var counter = Q { a: 1, b: 2, c: 3 };
const LIMITS: Pair<i64> = Pair<i64> { first: -5, second: 5 };

fn touch() { counter.b *= 2; counter.c++; }

fn main() {
    var p = mk(3);
    print(p.x, " ", p.y, " ", sum(p));
    bump(p);
    print(p);
    p = P { x: p.y, y: p.x };                   // reads before it writes
    print(p);

    var q = Q { a: 1, b: 2, c: 3 };
    q.b = q.b * 7;
    q.c += 5;
    q.a++;
    print(q, " ", widen(q), " ", q == Q { a: 2, b: 14, c: 8 }, " ", q != counter);
    q.b = -9223372036854775807 - 1;
    print(q.b, " ", q.a, " ", q.c);

    let r = R { a: 9, b: 100, c: 7 };
    print(r, " ", r.a + r.c, " ", r == R { a: 9, b: 100, c: 7 });

    var w = Wrap { p: p, q: q, tag: true, f: 1.5 };
    w.p.y = -1;
    w.q.b += 1;
    w.f *= 2.0;
    print(w);
    var w2 = w;
    w2.q.a = 200;
    print(w == w2, " ", w.q.a, " ", w2.q.a);

    print(ORIGIN, " ", E {}, " ", E {} == E {});
    touch();
    touch();
    print(counter, " ", LIMITS, " ", LIMITS.second - LIMITS.first);

    let s = swap(Pair<f64> { first: 1.25, second: -2.5 });
    print(s, " ", swap(Pair<u8> { first: 1, second: 2 }));
    print(Defaults { id: 7 }, " ", Defaults { id: 8, on: false }, " ", Defaults { 9, 0.5 });

    // Floats compare by value: -0.0 is 0.0, and NaN is not even itself.
    let zero = Floats { a: 0.0, b: -0.0 };
    let nan = 0.0 / 0.0;
    let fl = Floats { a: 1.0, b: nan };
    print(zero == Floats { a: -0.0, b: 0.0 }, " ", fl == fl, " ", fl != fl);

    var d = Deep { w: w, pairs: [Pair<i16> { first: 1, second: 2 }, Pair<i16> { first: 3, second: 4 }] };
    d.pairs[1].second = -4;
    d.w.q.c = 65535;
    print(d);
    var dp: Deep& = d;
    dp.pairs[0] = swap(dp.pairs[0]);
    print(d.pairs, " ", dp.w.q.c);
}
