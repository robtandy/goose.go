// A fixed-mode ADT a case-function call dispatches on is read before the
// later arguments run (§2, §8.2), through a reference too: its tag and its
// payload are the ones it had, whatever those arguments change.
enum E { A { v: i64 }, B { v: i64 } }
fn pick(e: E.A, x: i64) -> i64 { return e.v + x; }
fn pick(e: E.B, x: i64) -> i64 { return e.v * 2 + x; }

fn main() {
    var g: E = E.A { v: 1 };
    var r .= g;
    fn flip() -> i64 { g = E.B { v: 100 }; return 0; }
    print(pick(r, flip()));                 // 1
    print(pick(r, 0));                      // 200
    g = E.A { v: 1 };
    fn bump() -> i64 { g = E.A { v: 50 }; return 0; }
    print(pick(r, bump()));                 // 1
    print(pick(g, flip()));                 // 50
}
