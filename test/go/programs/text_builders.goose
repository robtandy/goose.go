// Text into u8[>..] builders (§3.7): str() builds a fresh one at its
// destination, format() appends to one, either straight at the builder's
// stack top; a value with no text of its own renders structurally, through
// a builder of its own for str().

struct V { x: f32, name: const u8[:] }
enum Shape { Dot, Circle { r: f64 } }

fn label(n: i64) -> u8[>..] { str("item", n) }

fn describe(out: u8[>..]&, v: V) {
    format(out, "<", v.name, ":", v.x, ">");
}

fn main() {
    let s = str("a", 1, "b", 2.5, true, 'c');
    print(s, " ", s.len);
    var b: u8[>..] = [];
    format(b, "x=", -7, " y=", 18446744073709551615 as u64, " ");
    format(b, V { x: 1.5, name: "v" });
    print(b, " ", b.len);
    describe(b, V { x: 0.1, name: "w" });
    print(b);
    print(label(3), " ", label(42).len);
    print(str([1, 2, 3], " ", Shape.Circle { r: 2.0 }, " ", Shape.Dot));
    let p: Shape = Shape.Circle { r: 0.5 };
    let q = str(p, [V { x: 2.0, name: "z" }]);
    print(q, " ", q.len);
    var lim: u8[..16] = [];
    format(lim, "n", 12, [1, 2]);
    print(lim, " ", lim.len);
    var acc: u8[>..<] = [];
    for i in 4 { format(acc, i, ","); }
    acc.pop();
    print(acc);
    // A builder grown through a slice of another.
    var t: u8[>..] = [];
    t.append(s[..2]);
    format(t, t.len);
    print(t);
}
