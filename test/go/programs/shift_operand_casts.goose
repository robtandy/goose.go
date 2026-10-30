// warning: redundant `as u64`: a u32 converts to u64 implicitly here
// warning: redundant `as u32`: a u8 converts to u32 implicitly here
// A cast that widens a shift's left operand is never redundant (issue #41).
// A shift computes at its left operand's type and masks its count to that
// type's width (§6.1, §6.2): with a u32 x, `(x as u64) << 32` is a 64-bit
// shift, where `x << 32` would be a u32 shift by 0, so the cast decides the
// value wherever the shift's value goes, and the checker calls no such cast
// redundant (§6.3). The two warnings above are all that a check of this
// program prints: each is about the cast of an operand that is not shifted,
// ORed with a shifted one of the wider type, which it converts to
// implicitly (`(lo as u64) | (x as u64) << 32`). The shifted operand's cast
// on the same line gets no warning.

fn take64(v: u64) -> u64 { v }
fn take32(v: u32) -> u32 { v }

// Returned at the type the cast gives.
fn high32(x: u32) -> u64 { (x as u64) << 32 }
fn high16(y: u8) -> u32 { return (y as u32) << 16; }

// x a u32 and y a u8 to shift, lo and z what is ORed in below their bits,
// s an i32, and the counts n32 and n16, which are no constants: x << n32 is
// a u32 shift by 0, y << n16 a u8 shift by 0.
fn shifts(x: u32, y: u8, lo: u32, z: u8, s: i32, n32: i64, n16: i64) {
    // The issue's two shifts, at a typed destination and ORed with 1, beside
    // the same shifts without the cast.
    let a: u64 = ((x as u64) << 32) | 1;
    let b: u64 = (x << n32) | 1;
    let c: u32 = ((y as u32) << 16) | 1;
    let d: u32 = (y << n16) | 1;
    print(a, " ", b, " ", c, " ", d);

    // Wherever the value goes: an inferred let, a wider destination, an
    // argument, a return, a compound assignment and a comparison.
    let e = (x as u64) << 32;
    let f: u64 = (y as u32) << 16;
    var g: u64 = 1;
    g |= (x as u64) << 32;
    var h: u32 = 1;
    h += (y as u32) << 16;
    print(e, " ", f, " ", g, " ", h, " ", ((x as u64) << 32) > 4294967295);
    print(take64((x as u64) << 32), " ", take32((y as u32) << 16), " ", high32(x), " ",
          high16(y));

    // Right shifts, counts that are no constants, and a signed operand.
    print((x as u64) >> 32, " ", x >> n32, " ", (x as u64) << n32, " ", (y as u32) << n16);
    let t: i64 = (s as i64) << 32;
    let u: i64 = s << n32;
    print(t, " ", u);

    // Bytes assembled into a wider integer: the cast of the operand that is
    // not shifted converts implicitly, and warns; the shifted one's does not.
    let w: u64 = (lo as u64) | (x as u64) << 32;
    let v: u32 = (z as u32) | (y as u32) << 16;
    print(w, " ", v);
}

fn main() {
    shifts(0xFFFFFFFF, 0xFF, 0x12345678, 0x9A, -1, 32, 16);
    shifts(0x80000001, 0x81, 7, 3, 5, 32, 16);
}
