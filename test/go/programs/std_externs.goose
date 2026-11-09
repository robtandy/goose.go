// std's extern fns that the C runtime implements (popcount, clz, ctz,
// find_any, find_pair, frexp, ldexp, and since issue #1 the rotations, byte
// swaps and shortest_digits), through std's own functions.
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
    // std's rotations and byte swaps (upstream's 75ad5bd3, issue #1): math/bits'
    // under --target=go. A count is taken modulo the width, of any integer
    // type; a negative one turns the other way.
    var ws: u64[>..] = [0x8000000000000001, 0x0102030405060708];
    let w32 = ws[0] as! u32 | 0x80000000;
    let n: i64 = -3;
    let mn = -9223372036854775807 - 1;
    print(rotl(w32, 1), " ", rotr(w32, 1), " ", rotl(w32, n), " ", rotr(w32, 35), " ", rotl(w32, mn));
    print(rotl(ws[0], 1), " ", rotr(ws[0], 4), " ", rotl(ws[1], 72), " ", rotr(ws[1], n), " ", rotr(ws[1], mn));
    let h: u16 = 0x1234;
    print(byte_swap(h), " ", byte_swap(ws[1] as! u32), " ", byte_swap(ws[1]), " ", byte_swap(byte_swap(ws[0])) == ws[0]);
    // The shortest digits print writes, json's fallback: their count, and the
    // power of ten they are a fraction of.
    var digits: u8[17] = [0; 17];
    var point: i64 = 99;
    for v in [0.1, 123.45, -2.5e-300, 1.7976931348623157e308, 0.0] {
        let k = float_text::shortest_digits(v, digits[..], point);
        print(k, " ", digits[..k], " ", point);
    }
}
