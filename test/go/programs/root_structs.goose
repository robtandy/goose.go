// Root structs (issue #59): owners whose components lie apart, each where a
// variable of its type would -- a resizable or a bytes value on a data stack
// of its own, a fixed-size field a Go local or global -- with a descriptor of
// references to them, which a reference to the owner points at. Locals and
// globals, generic owners, owners handed to calls, returned through, held
// by recursion, aliased, captured by a nested function and printed.

struct P { x: i32, y: i32 }

root struct State {
    items: i64[>..] = [],
    work:  i64[>..<] = [],
    count: i64       = 0,
    name:  u8[]      = "state",
    code:  varint    = 0,
    last:  P         = P { 0, 0 },
}

root struct Pair<T> { a: T[>..], b: T[>..] }

var global = State { .. };
var gpair = Pair<u8> { "ab", "cd" };

fn fill(s: State&, n: i64) {
    for i in n { s.items.push(i * 10); }
    s.work.push(n);
    s.count += n;
    s.last = P { n as i32, (n * 2) as i32 };
}

fn total(s: const State&) -> i64 {
    var t = 0;
    for x in s.items { t += x; }
    return t + s.count;
}

fn first_ref(s: State&) -> i64& { s.items[0] }

recursive fn depth(s: State&, n: i64) -> i64 {
    if n == 0 { return s.work.len; }
    s.work.push(n);
    let r = depth(s, n - 1);
    s.work.pop();
    return r;
}

// No reference to the owner is taken: its components are plain locals.
fn local_only() -> i64 {
    var s = State { count: 5, .. };
    s.items.push(1);
    s.items.push(2);
    s.count += s.items.len;
    return s.count + s.items[1];
}

fn move_one<T>(p: Pair<T>&) {
    let t = p.a.pop();
    p.b.push(t);
}

fn main() {
    var s = State { [1, 2], [3], 7, "local", 300, P { 1, 2 } };
    fill(s, 3);
    print(s.items, " ", s.work, " ", s.count, " ", s.name, " ", s.code, " ", s.last.x, " ", s.last.y);
    print(total(s), " ", local_only());
    let r .= first_ref(s);
    r = 99;
    print(s.items[0], " ", depth(s, 4), " ", s.work.len);
    fill(global, 2);
    print(global.items, " ", total(global), " ", global.name, " ", global.code);
    move_one(gpair);
    print(gpair.a, " ", gpair.b);
    var lp = Pair<i64> { [1, 2, 3], [] };
    move_one(lp);
    move_one(lp);
    print(lp.a, " ", lp.b);
    // An alias of the owner, and a nested function reaching it.
    let alias = s;
    alias.items.push(5);
    fn bump() { s.count += 100; s.work.push(s.count); }
    bump();
    print(s.items.len, " ", s.count, " ", s.work);
    print(s);
}
