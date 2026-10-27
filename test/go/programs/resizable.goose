// Resizable arrays on data stacks (§5.1, §5.2): a header in the frame, the
// elements at the top of a stack of their own, growth a bump of the top and
// the count, and the stack's top rewound as each scope ends.

struct P { x: i32, y: i32 }
struct Tag { name: const u8[:], n: u8 }

fn sum(xs: i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    return s;
}

fn ops() {
    var a: i64[>..] = [];
    a.push(1);
    a.push(2);
    a.push(3);
    print(a.len, " ", a[0], " ", a[2], " ", a);
    let p = a.pop();
    print(p, " ", a);
    a.resize(5, 7);
    print(a);
    a.pop_n(2);
    print(a, " ", a.len);
    a.resize(1);
    print(a);
    a.clear();
    print(a.len, " ", a);
    var g: i64[>..<] = [4, 5, 6];
    g.append([7, 8]);
    g.append(g[..2]);
    let fixed: i64[3] = [10, 11, 12];
    g.append(fixed);
    print(g, " ", g[1..3], " ", g[g.len - 2..], " ", sum(g));
}

fn elements() {
    var ps: P[>..] = [];
    for i in 4 { ps.push(P { x: i as i32, y: (i * i) as i32 }); }
    let q: P& = ps.push(P { x: 9, y: 9 });
    q.y = 90;
    for &p in ps { p.x += 100; }
    var t = 0;
    for p in ps { t += p.y; }
    print(ps, " ", t, " ", ps.index_of(ps[2]), " ", ps.index_of(q));
    // Elements that are slices of string literals, and of other strings.
    var tags: Tag[>..] = [];
    tags.push(Tag { name: "alpha", n: 1 });
    tags.push(Tag { name: "beta", n: 2 });
    var names: (const u8[:])[>..] = [];
    for &tg in tags { names.push(tg.name); }
    names.push(tags[0].name[1..3]);
    print(tags, " ", names, " ", names.len);
    // Bytes.
    var s: u8[>..] = [];
    for c in "goose" { s.push(c - 32); }
    s.append(" and gander");
    print(s, " ", s.len, " ", s[0]);
}

fn equality() {
    var a: i64[>..] = [1, 2, 3];
    var b: i64[>..] = [1, 2];
    print(a == b, " ", a != b);
    b.push(3);
    print(a == b, " ", a != b);
    var c: P[>..] = [P { x: 1, y: 2 }];
    var d: P[>..] = [P { x: 1, y: 2 }];
    print(c == d);
    d[0].y = 3;
    print(c == d);
    var f: f64[>..<] = [0.5, -0.0];
    var h: f64[>..<] = [0.5, 0.0];
    print(f == h);
}

// Whole-array assignment empties the array and builds the new contents in
// its place (§4.4); copy() is a copy of the elements.
fn assignment() {
    var a: i64[>..] = [1, 2, 3];
    var b: i64[>..] = [];
    b = copy(a);
    b.push(4);
    print(a, " ", b);
    b = [9];
    print(b, " ", b.len);
    b = a[1..];
    print(b);
    var c: i64[>..<] = copy(b);
    c.pop();
    print(c, " ", b);
}

// A nested function reaches the arrays of the function around it.
fn captured() {
    var a: i64[>..] = [1];
    var n = 0;
    fn add(k: i64) {
        a.push(k);
        n += 1;
    }
    add(5);
    add(6);
    print(a, " ", n, " ", sum(a));
}

// A loop's own arrays start empty on every iteration, their stacks rewound
// as the iteration ends, by any exit.
fn scopes() {
    var total = 0;
    for i in 6 {
        var tmp: i64[>..] = [];
        for j in i { tmp.push(j); }
        if i == 4 { continue; }
        total += sum(tmp) * 10 + tmp.len;
        if i == 5 { break; }
    }
    print(total);
    var k = 0;
    while k < 3 {
        var w: u8[>..<] = [];
        w.append("ab");
        k += w.len;
    }
    print(k);
}

fn main() {
    ops();
    elements();
    equality();
    assignment();
    captured();
    scopes();
}
