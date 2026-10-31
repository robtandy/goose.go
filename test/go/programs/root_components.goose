// A root owner's components of every kind (issue #59): a fixed-size value
// larger than a native one, which C holds on a data stack of its own, a
// frame object, a variable-size struct and a grow-shrink array, beside a
// default; owners declared in a loop, whose components' stacks each
// iteration gives back, and a callee's defer, through the owner's
// reference, as the owner and its components are handed on.
struct Tail { tag: i64, xs: i64[>..] }
struct Var { name: u8[], n: i32 }
root struct R {
    big: u8[5000],
    tail: Tail,
    v: Var,
    rows: i64[>..<] = [],
    flag: bool = true,
}
var g = R { [7; 5000], Tail { 3, [1, 2] }, Var { "gv", 9 }, .. };

fn touch(r: R&, k: i64) -> i64 {
    r.big[k] = (k % 200) as u8;
    r.tail.xs.push(k);
    r.rows.push(r.tail.tag + k);
    defer { r.rows.push(-1); }
    if k > 2 { return r.big[k] as i64 + r.v.n; }
    r.tail.xs.len
}

fn main() {
    var total = 0;
    for i in 4 {
        var r = R { [i as u8; 5000], Tail { i, [] }, Var { "lv", 2 }, .. };
        total += touch(r, i);
        total += r.rows.len + r.tail.xs.len + r.big[0];
        print(r.v.name, " ", r.rows, " ", r.flag);
    }
    total += touch(g, 3) + touch(g, 1);
    print(total, " ", g.tail.xs, " ", g.rows, " ", g.v.name, " ", g.big[3]);
}
