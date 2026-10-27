// Long-distance returns (§7.9): `return ... from f` out of the functions
// between, and a block's return out of the function it is written in,
// through the calls it is handed to: fixed, resizable and variable-array
// results, one call's results forwarded, the exits of every frame run.
fn each<F>(n: i64) { for i in n { F(i); } }

fn first_square_above(n: i64, limit: i64) -> i64 {
    each(n) { i => if i * i > limit { return i * i; } };
    return -1;
}

recursive fn dive(depth: i64, at: i64) {
    defer { print("leaving ", depth); }
    if depth == at { return depth * 100 from search; }
    dive(depth + 1, at);
}
fn search(at: i64) -> i64 { dive(0, at); return -1; }

recursive fn grow(depth: i64) {
    {
        var scratch: u8[>..] = [];
        scratch.append("tmp");
    }
    if depth == 3 {
        var out: u8[>..] = [];
        for i in 5 { out.push(('a' + i) as u8); }
        return out from collect;
    }
    grow(depth + 1);
}
fn collect() -> u8[>..] {
    var mine: u8[>..] = [];
    mine.append("never");
    grow(0);
    return mine;
}

fn pair() -> i64, u8[] { return 7, "seven"; }
recursive fn relay(n: i64) { if n == 0 { return pair() from both; } relay(n - 1); }
fn both() -> i64, u8[] { relay(4); return 0, "none"; }

fn label(k: i64) -> u8[] {
    each(3) { i => if i == k { return str("found ", i); } };
    return "missing";
}

fn main() {
    print(first_square_above(10, 20), " ", first_square_above(3, 100));
    print(search(3));
    let c = collect();
    print(c, " ", c.len);
    let n, s = both();
    print(n, " ", s);
    print(label(1), " ", label(5));
    var t = 0;
    for k in 6 { t += search(k % 4); }
    print(t);
}
