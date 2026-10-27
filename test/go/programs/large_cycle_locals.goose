// A fixed-size local of more than 4096 bytes inside a recursive cycle lives
// in its function's frame (§7.8): each activation's own, intact across the
// deeper calls, and given back as each returns, so the depth takes no more
// than the frames in use.
struct Big { tag: i64, data: u8[6000] }

recursive fn walk(n: i64) -> i64 {
    var b = Big { tag: n, data: [0; 6000] };
    for i in 6000 { b.data[i] = ((i + n) % 251) as u8; }
    if n == 0 { return b.data[5999]; }
    let below = walk(n - 1);
    var s = 0;
    for i in 6000 { s += b.data[i]; }
    return below + s % 1000 + b.tag;
}

fn main() {
    print(walk(40));
    var t = 0;
    for k in 200 { t += walk(3) - k % 2; }
    print(t);
}
