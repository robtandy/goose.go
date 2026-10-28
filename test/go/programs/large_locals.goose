// Fixed-size locals of more than 4096 bytes, which the C backend holds on a
// data stack each, as it holds the temporaries their literals and defaults
// are built in (FixedLocal): so does the Go backend's, which takes the same
// stacks (--stacks). A local declared in a loop or a branch gives its stack
// back at its block's end, which the next one takes again.
struct Packet { bytes: u8[5000], tag: i64 }
struct Pair { a: Packet, b: Packet }

fn bump(p: Packet&) {
    p.tag += 1;
    p.bytes[4999] = 9;
}

fn main() {
    var grid: u8[5000] = [0; 5000];
    grid[3] = 1;
    var a = Packet { tag: 1, .. };
    var b = a;
    b.tag = 5;
    b.bytes[0] = 2;
    a = b;
    bump(a);
    print(grid[3], " ", a.tag, " ", b.tag, " ", a.bytes[0], " ", a.bytes[4999], " ", b.bytes[4999]);
    let d = default<Packet>();
    print(d.tag, " ", d.bytes.len, " ", d.bytes[17]);
    for i in 3 {
        var loc = Packet { tag: i, .. };
        loc.bytes[i] = 7;
        print(loc.tag, " ", loc.bytes[i], " ", loc.bytes[i + 1]);
    }
    var pr = Pair { a: a, b: Packet { tag: 2, .. } };
    pr.b.tag += 40;
    let r = &pr.a;
    print(pr.a.tag, " ", pr.b.tag, " ", r.tag, " ", r.bytes[4999]);
    if a.tag > 3 {
        let e = if a.tag > 5 { b } else { a };
        print(e.tag);
    }
    var arr: u8[6000] = [3; 6000];
    var s = 0;
    for x in arr { s += x; }
    var g = 0;
    for x in grid[..5] { g += x; }
    print(s, " ", arr.len, " ", g);
}
