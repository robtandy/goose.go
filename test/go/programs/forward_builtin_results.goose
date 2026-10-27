// A return forwarding a builtin's results as the function's own (PR #10's
// review): from_bytes's array and whether the image held one, returned
// whole, by functions that are and are not inlined.
fn decode(img: const u8[:]) -> i64[>..], bool {
    return from_bytes<i64[>..]>(img);
}

recursive fn decode_deep(img: const u8[:], depth: i64) -> i64[>..], bool {
    if depth > 0 { return decode_deep(img, depth - 1); }
    return from_bytes<i64[>..]>(img);
}

fn sum(xs: const i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    s
}

fn main() {
    var xs: i64[>..] = [3, 1, 4, 1, 5, 9, 2, 6];
    var img: u8[>..] = xs.to_bytes();
    var back, ok = decode(img);
    print(ok, " ", back.len, " ", sum(back), " ", back[..]);
    back.push(7);
    print(back.len, " ", back[back.len - 1]);
    var deep, dok = decode_deep(img, 3);
    print(dok, " ", deep.len, " ", sum(deep));
    var cut, cok = decode(img[..3]);
    print(cok, " ", cut.len);
    var none, nok = decode_deep("", 2);
    print(nok, " ", none.len);
}
