// Case-function dispatch over a variable-mode ADT (§8.2): a case taking its
// variant by reference works on the value in place; one taking it by value
// gets a copy taken before the later arguments run, the whole image's where
// a by-value case takes a variable-size payload, each fixed payload's
// otherwise.
enum Mixed { Num { n: i64 }, Text { s: u8[] }, Nothing }
fn show(m: Mixed.Num&, k: i64) -> i64 { m.n += k; return m.n; }
fn show(m: Mixed.Text, k: i64) -> i64 { print(m.s); return m.s.len + k; }
fn show(m: Mixed.Nothing, k: i64) -> i64 { return k; }

enum Fixedish { A { a: i64 }, B { b: i64, c: i64 } }
fn take(f: Fixedish.A, x: i64) -> i64 { return f.a * 10 + x; }
fn take(f: Fixedish.B&, x: i64) -> i64 { f.b += x; return f.b + f.c; }

fn main() {
    var a: Mixed.. = Mixed.Num { n: 3 };
    print(show(a, 4), " ", show(a, 1));
    match &a { Num &v => print(v.n), _ => {} }
    var t: Mixed.. = Mixed.Text { s: "hello" };
    print(show(t, 2));
    let z: Mixed.. = Mixed.Nothing;
    print(show(z, 9));
    var f: Fixedish.. = Fixedish.A { a: 4 };
    print(take(f, 2));
    var g: Fixedish.. = Fixedish.B { b: 1, c: 100 };
    print(take(g, 5), " ", take(g, 5));
}
