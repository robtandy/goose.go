// std's extern fns that the C runtime implements (popcount, clz, ctz,
// find_any, find_pair, frexp, ldexp), through std's own functions.
import std;

fn main() {
    print(popcount(0), " ", popcount(255), " ", popcount(0xFFFFFFFFFFFFFFFF));
    print(clz(0), " ", clz(1), " ", clz(1 << 40), " ", ctz(0), " ", ctz(8), " ", ctz(0x8000000000000000));
    let digits = byte_set("0123456789");
    let space = byte_set(" \t");
    print(find_any("abc7d", digits), " ", find_any("abcd", digits), " ", find_any("", digits));
    print(find_pair("a1 b2 c", digits, 1, space), " ", find_pair("a1b2", digits, 1, space));
    var xs: i64[>..] = [];
    for i in 40 { xs.push((i * 37 + 11) % 23); }
    sort(xs[..]);
    print(xs[0], " ", xs[20], " ", xs[39]);
    let v, ok = parse_flt("3.25e2");
    print(v, " ", ok);
    let w, ok2 = parse_flt("1e-320");
    print(w, " ", ok2);
    let x, ok3 = parse_flt("123456789012345678901234567890");
    print(x, " ", ok3);
    var out: u8[>..] = [];
    format_flt(out, 2.5, 3);
    print(out);
}
