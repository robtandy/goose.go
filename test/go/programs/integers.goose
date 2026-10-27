// Integer semantics at every width (§6.2): arithmetic that wraps in a
// release build, Euclidean `%`, shifts whose count is masked to the width,
// bitwise operators, comparisons, and conversions with `as` and `as!`.
// The values come through functions, so that the optimizer folds none of
// them, and as constants, which it folds or leaves to the backend.

fn id8(x: i8) -> i8 { x }
fn id16(x: i16) -> i16 { x }
fn id32(x: i32) -> i32 { x }
fn id64(x: i64) -> i64 { x }
fn idu8(x: u8) -> u8 { x }
fn idu16(x: u16) -> u16 { x }
fn idu32(x: u32) -> u32 { x }
fn idu64(x: u64) -> u64 { x }

fn main() {
    // Wrapping at each width.
    print(id8(127) + 1, " ", id8(-128) - 1, " ", id8(100) * 3, " ", -id8(-128));
    print(id16(32767) + 1, " ", id16(-32768) - 1, " ", id16(300) * 300);
    print(id32(2147483647) + 1, " ", id32(-2147483647) - 2, " ", id32(65536) * 65536);
    print(id64(9223372036854775807) + 1, " ", id64(-9223372036854775807) - 2);
    print(idu8(255) + 1, " ", idu8(0) - 1, " ", idu8(16) * 16, " ", idu8(200) + idu8(100));
    print(idu16(65535) + 1, " ", idu16(0) - 1, " ", idu32(4294967295) + 1, " ", idu32(0) - 1);
    print(idu64(18446744073709551615) + 1, " ", idu64(0) - 1, " ", idu64(4294967296) * 4294967296);

    // Constants that wrap: the optimizer leaves signed overflow to run time.
    let a: i8 = 100;
    let b: i8 = a + a;
    let c: i32 = 2147483647;
    let d: i32 = c * 2;
    let e: i64 = -9223372036854775807 - 1;
    print(b, " ", d, " ", e, " ", e - 1, " ", -e);

    // Division truncates toward zero; `%` is Euclidean, never negative.
    for x in -7..8 {
        if x == 0 { continue; }
        print(x, ": ", 7 / x, " ", -7 / x, " ", 7 % x, " ", -7 % x, " ", id8(-128) % id8(x as i8));
    }
    print(id8(-128) / id8(-1), " ", id16(-32768) / -1, " ", id32(-2147483648) / -1);
    print(idu8(200) / 7, " ", idu8(200) % 7, " ", idu64(18446744073709551615) / 10, " ",
          idu64(18446744073709551615) % 10);
    print(id64(-9223372036854775807 - 1) % -1, " ", id64(5) % (-9223372036854775807 - 1), " ",
          id64(-5) % (-9223372036854775807 - 1));

    // Shifts: the count is taken modulo the width.
    var n = 0;
    while n < 70 {
        print(n, ": ", id8(1) << n, " ", id8(-128) >> n, " ", idu8(255) >> n, " ",
              id32(1) << n, " ", id64(-1) << n, " ", idu64(18446744073709551615) >> n);
        n += 9;
    }
    print(1 << 62, " ", id64(3) << 63, " ", -(id64(1) << 63) >> 63);

    // Bitwise operators.
    print(id8(-86) & 0x0F, " ", idu8(0xA5) | 0x5A, " ", idu16(0xFFFF) ^ 0x00FF, " ", ~id32(0),
          " ", ~idu8(0), " ", ~idu64(0), " ", ~id64(-9223372036854775807 - 1));

    // Comparisons, signed and unsigned.
    print(id8(-1) < 0, " ", idu8(255) > 0, " ", idu64(18446744073709551615) > 9223372036854775807,
          " ", id32(5) <= 5, " ", id16(-3) >= -2, " ", idu32(7) == 7, " ", id64(8) != 8);

    // Conversions: as! wraps and truncates, as widens and narrows values
    // that fit.
    let big = id64(1234567890123);
    print(big as! i8, " ", big as! u8, " ", big as! i16, " ", big as! u16, " ", big as! i32, " ",
          big as! u32, " ", big as! u64);
    print(id64(-1) as! u64, " ", idu64(18446744073709551615) as! i64, " ", id8(-1) as! u32, " ",
          idu8(255) as i64, " ", id32(-5) as i64, " ", idu32(4000000000) as u64);
    print(id64(100) as i8, " ", id64(-100) as i8, " ", idu64(65535) as u16, " ", id16(-1) as i64);
    print(300 as! u8, " ", (-1) as! u8, " ", 70000 as! i16, " ", 65 as u8);

    // Compound assignment and ++/--, at narrow types.
    var x: u8 = 250;
    x += 10;
    print(x);
    x -= 20;
    print(x);
    x *= 3;
    print(x);
    x /= 7;
    print(x);
    x %= 5;
    print(x);
    x <<= 7;
    print(x);
    x >>= 3;
    print(x);
    x |= 1;
    x &= 0xF1;
    x ^= 0xFF;
    print(x);
    var y: i16 = 32767;
    y++;
    print(y);
    y--;
    print(y);
    var z: i64 = 0;
    z--;
    print(z, " ", z * z);
}
