// Resizable values across calls (§7.2-§7.4, C.3): a reference to one is its
// header's address and its stack (GsRref), through which a callee grows the
// caller's array; a by-value one is built on a stack of the call's own; a
// result is built at the destination the caller hands over, a local the
// returns name built there from its declaration on (§7.3).

struct Cell { v: i64, w: i32 }

fn fill(out: i64[>..]&, n: i64) {
    for i in n { out.push(i * 10); }
}

fn drop2(xs: i64[>..<]&) -> i64 {
    xs.pop_n(2);
    return xs.len;
}

fn grow(out: Cell[>..]&, k: i64) -> Cell& { out.push(Cell { v: k, w: k as i32 * 2 }) }

// By value: the callee owns a copy, and may grow it.
fn consume(xs: i64[>..]) -> i64 {
    var a = xs;
    a.push(100);
    var s = 0;
    for x in a { s += x; }
    return s;
}

// The named result is built at the destination.
fn squares(n: i64) -> i64[>..] {
    var r: i64[>..] = [];
    for i in n { r.push(i * i); }
    return r;
}

// Another value than the named result, built behind it and moved down.
fn evens(n: i64, empty: bool) -> i64[>..] {
    var r: i64[>..] = [];
    for i in n { r.push(i * 2); }
    if empty { return []; }
    return r;
}

// Results that are not a local, and several of them.
fn pair(n: i64) -> i64[>..<], i64 {
    return [n, n + 1, n + 2], n * 3;
}

fn concat(a: i64[:], b: i64[:]) -> i64[>..] {
    var r: i64[>..] = [];
    r.append(a);
    r.append(b);
    r
}

// Forwarding another function's results.
fn pair2(n: i64) -> i64[>..<], i64 { return pair(n + 1); }

recursive fn countdown(out: i64[>..]&, n: i64) {
    if n == 0 { return; }
    out.push(n);
    countdown(out, n - 1);
}

var log: i64[>..] = [];
var cells: Cell[>..] = [Cell { v: 1, w: 1 }];

fn note(k: i64) { log.push(k); }

fn main() {
    var a: i64[>..] = [];
    fill(a, 3);
    fill(a, 2);
    print(a, " ", a.len);
    var g: i64[>..<] = [1, 2, 3, 4, 5];
    print(drop2(g), " ", g);
    var cs: Cell[>..] = [];
    let c0: Cell& = grow(cs, 4);
    c0.v = 40;
    grow(cs, 5);
    print(cs);
    print(consume(copy(a)), " ", a);
    let sq = squares(5);
    print(sq, " ", sq.len, " ", squares(3));
    print(evens(4, false), " ", evens(4, true), " ", evens(2, false).len);
    let p, k = pair(7);
    print(p, " ", k);
    var q, m = pair2(1);
    q.push(m);
    print(q);
    print(concat(sq[..2], a[3..]));
    var cd: i64[>..] = [];
    countdown(cd, 4);
    print(cd);
    note(3);
    note(4);
    fill(log, 2);
    cells.push(Cell { v: 2, w: 2 });
    grow(cells, 3);
    print(log, " ", cells);
}
