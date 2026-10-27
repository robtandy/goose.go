// User format overloads (§3.7): print, str and format hand each value of
// the overloaded type to its `format(out, v)`, at any depth of a value, the
// builder the text goes into whole -- so an overload that clears it clears
// what came before it -- and by reference where it takes one, the storage
// the value lies in: what a call returning a reference refers to.
struct P { x: i64, y: i64 }
struct Box { name: const u8[:], at: P }
struct Counter { n: i64 }
struct Reset { k: i64 }
var gc = Counter { n: 0 };

fn format(out: u8[>..]&, p: P) { format(out, "(", p.x, ",", p.y, ")"); }
fn format(out: u8[>..]&, c: Counter&) { c.n += 1; format(out, "#", c.n); }
fn format(out: u8[>..]&, r: Reset) { out.clear(); format(out, "reset", r.k); }
fn format(out: u8[>..]&, v: i32) { out.append("<"); out.format(v as i64 * 2); out.append(">"); }
fn counter() -> Counter& { return gc; }

fn main() {
    let p = P { x: 1, y: 2 };
    print(p, " ", Box { name: "b", at: P { x: 3, y: 4 } });
    let ps: P[3] = [P { x: 5, y: 6 }, P { x: 7, y: 8 }, P { x: 9, y: 10 }];
    print(ps);
    print(counter(), " ", counter(), " ", gc.n);
    var c = Counter { n: 10 };
    print(c, " ", c.n);
    print("gone ", Reset { k: 1 }, " kept");
    var b: u8[>..] = [];
    b.append("start ");
    format(b, Reset { k: 2 }, " then ", p);
    print(b);
    let m: i32 = 21;
    print(m, " ", str(m, " ", p), " ", 5);
    var lim: u8[..32] = [];
    format(lim, p, m);
    print(lim);
}
