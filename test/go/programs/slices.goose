// Slices T[:] (§3.10) and string literals (§3.7): a GsSlice of the first
// element's address and the count. Sub-slicing with every bound form,
// indexing, iteration, slices of arrays, of limited arrays and of slices,
// slices as parameters and fields, equality (of the elements at the top of
// an ==, of the slice itself inside a value), and the text of print.

struct Pt { x: i64, y: i64 }
struct View { name: const u8[:], data: i64[:] }

fn first(s: const u8[:]) -> u8 { s[0] }
fn count(s: const u8[:], c: u8) -> i64 { var n = 0; for b in s { if b == c { n += 1; } } n }
fn sum(s: const i64[:]) -> i64 { var t = 0; for x in s { t += x; } t }
fn scale(s: i64[:], k: i64) { for &x in s { x *= k; } }
fn mid(s: i64[:]) -> i64[:] { s[1..^1] }
fn words(s: const u8[:]) -> i64 {
    var n = 0;
    var inword = false;
    for c in s {
        if c == ' ' { inword = false; } else if !inword { inword = true; n += 1; }
    }
    n
}

fn main() {
    let msg = "hello, world";
    print(msg, " ", msg.len, " ", first(msg), " ", count(msg, 'o'), " ", words("  a bb  ccc "));
    print(msg[7..], "|", msg[..5], "|", msg[^5..], "|", msg[2..^2], "|", msg[3..3], "|");
    print(msg == "hello, world", " ", msg[0..5] == "hello", " ", msg != "hello", " ", "" == msg[4..4]);

    var a = [5, 6, 7, 8];
    print(sum(a), " ", sum(a[1..3]), " ", a[1..3], " ", a[..0]);
    scale(a[2..], 10);
    print(a, " ", mid(a), " ", sum(mid(a)));
    var s: i64[:] = a;
    s = s[1..];
    s[0] = -1;
    print(s, " ", s.len, " ", a);
    var n = 0;
    while s.len > 0 { n += s[0]; s = s[1..]; }
    print(n);

    var lim: i64[..6] = [1, 2, 3];
    let ls: i64[:] = lim;
    print(ls, " ", sum(lim[1..]), " ", ls == [1, 2, 3]);

    let pts = [Pt { x: 1, y: 2 }, Pt { x: 3, y: 4 }, Pt { x: 5, y: 6 }];
    let ps: const Pt[:] = pts;
    var t = 0;
    for p in ps[1..] { t += p.x * p.y; }
    print(t, " ", ps[2], " ", ps, " ", ps[0..2] == pts[0..2]);

    // Inside a value a slice compares as itself: its address and its count.
    let v1 = View { name: "v", data: a };
    let v2 = View { name: "v", data: a[0..] };
    let v3 = View { name: "v", data: a[1..] };
    print(v1 == v2, " ", v1 == v3, " ", v1.data == v3.data[0..], " ", v1);

    let fs = [0.0, 1.5];
    let gs = [-0.0, 1.5];
    let fsl: const f64[:] = fs;
    print(fsl == gs, " ", fsl[1..] == gs[1..]);
    let nested: (const u8[:])[2] = ["tab\there", "quote\"d"];
    print(nested, " ", nested[0], " ", nested[1].len);
}
