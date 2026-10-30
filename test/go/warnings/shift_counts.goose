// warning: shift of u8 by 24 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(BYTE as u32) << 24`
// warning: shift of u8 by 8 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(x as u16) << 8`
// warning: shift of u8 by 8 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(b as u16) << 8`
// warning: shift of u16 by 16 is a shift by 0: at u16 the count is masked to 0..15 (§6.2); cast the operand first: `(h as u32) << 16`
// warning: shift of u32 by 32 is a shift by 0: at u32 the count is masked to 0..31 (§6.2); cast the operand first: `(w as u64) << 32`
// warning: shift of u8 by 16 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(b as u32) << 16`
// warning: shift of u8 by 16 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(b as u32) << SHIFT`
// warning: shift of u16 by 24 is a shift by 8: at u16 the count is masked to 0..15 (§6.2); cast the operand first: `(h as u64) << (8 * 3)`
// warning: shift of u16 by 16 is a shift by 0: at u16 the count is masked to 0..15 (§6.2); cast the operand first: `(b as u32) << 16`
// warning: shift of u64 by 64 is a shift by 0: at u64 the count is masked to 0..63 (§6.2)
// warning: shift of i64 by 70 is a shift by 6: at i64 the count is masked to 0..63 (§6.2)
// warning: shift of u32 by 32 is a shift by 0: at u32 the count is masked to 0..31 (§6.2)
// warning: shift of u8 by 8 is a shift by 0: at u8 the count is masked to 0..7 (§6.2)
// warning: shift of u8 by -1 is a shift by 7: at u8 the count is masked to 0..7 (§6.2)
// warning: shift of u8 by 8 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(it as u16) << 8`
// Shifts by a constant count outside the shifted type's width (issue #27).
// A shift computes at its left operand's type, and its count is masked to
// that type's width (§6.1, §6.2): with a u8 b, `b << 8` is `b << 0`, and
// `let v: u32 = b << 16;` stores b itself where `(b as u32) << 16` was
// meant. The checker lets such a shift pass, and --target=go warns of each
// one, once, at every -O level, as the lines above say; of nothing else
// here: not of a shift whose operand is cast first, a count in range, or a
// count that is no constant.

let SHIFT = 16;        // A named constant (§3.1), which a count can be.
let BYTE: u8 = 0xAB;   // A global of a type of its own, which is no constant.
let WORD: u32 = BYTE << 24;

// One shift as the source has it, at two types: warned of at u8 alone.
fn high<T>(x: T) -> T { x << 8 }

fn apply<F>(x: u8) -> u8 { F(x) }

fn main() {
    var b: u8 = 0xAB;
    var h: u16 = 0xABCD;
    var w: u32 = 0xDEADBEEF;
    var q: u64 = 0x0123456789ABCDEF;
    var k = 3;   // A count that is no constant.

    // A shift by its type's width is a shift by 0 ...
    let s8 = b << 8;
    let s16 = h << 16;
    let s32 = w << 32;
    // ... where the operand cast first is shifted as the source says.
    let c8 = (b as u16) << 8;
    let c16 = (h as u32) << 16;
    let c32 = (w as u64) << 32;
    print(s8, " ", s16, " ", s32, " / ", c8, " ", c16, " ", c32);

    // A value widened at once names the type to cast to.
    let v: u32 = b << 16;
    let m: u32 = (b as u32) << 16;
    let n = b << SHIFT;
    let o: u64 = h << (8 * 3);
    // An operand cast first, but to too narrow a type, is cast wider.
    let t: u32 = (b as u16) << 16;
    print(v, " ", m, " ", n, " ", o, " ", t);

    // No wider type, a constant's i64, a right shift, a compound assignment
    // and a negative count: what the shift does instead, and nothing to cast.
    let s64 = q << 64;
    let big = 1 << 70;
    let r32 = w >> 32;
    var e: u8 = 1;
    e <<= 8;
    let neg = b >> -1;
    print(s64, " ", big, " ", r32, " ", e, " ", neg);

    // Counts in range, and counts that are no constants.
    print(b << 7, " ", w >> 31, " ", q << 63, " ", b << k, " ", w >> (k * 11));

    // A generic function's shift at each type it is called at, a function
    // value's, and a global's.
    let f = apply(b) { it << 8 };
    print(high(b), " ", high(h), " ", f, " ", WORD);
}
