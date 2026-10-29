// A module of wide_bytes.goose and wide_bytes_debug.goose (issue #23): integers
// assembled from the bytes of consecutive elements of a u8 array or slice,
// and taken apart into them. With every byte's bounds proven, the Go backend
// makes each such expression one load and each such loop or run of stores one
// store of 2, 4 or 8 bytes, little- or big-endian (src/codegen_go_wide.h).
// Every width and order is read and written at every offset of slices of
// several lengths, the last bytes included, in each form the backend
// recognizes and in forms it leaves byte by byte, against values worked out a
// byte at a time with no OR in them.
import std;

// ---- loads, the span proven by a guard

fn le16(s: const u8[:], i: i64) -> u16 {
    if i < 0 || i + 2 > s.len { return 0; }
    s[i] | (s[i + 1] as u16) << 8
}
fn be16(s: const u8[:], i: i64) -> u16 {
    if i < 0 || i + 2 > s.len { return 0; }
    (s[i] as u16) << 8 | s[i + 1]
}
fn le32(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 4 > s.len { return 0; }
    s[i] | (s[i + 1] as u32) << 8 | (s[i + 2] as u32) << 16 | (s[i + 3] as u32) << 24
}
fn be32(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 4 > s.len { return 0; }
    (s[i] as u32) << 24 | (s[i + 1] as u32) << 16 | (s[i + 2] as u32) << 8 | s[i + 3]
}
fn le64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i + 8 > s.len { return 0; }
    s[i] | (s[i + 1] as u64) << 8 | (s[i + 2] as u64) << 16 | (s[i + 3] as u64) << 24 |
        (s[i + 4] as u64) << 32 | (s[i + 5] as u64) << 40 | (s[i + 6] as u64) << 48 |
        (s[i + 7] as u64) << 56
}
fn be64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i + 8 > s.len { return 0; }
    (s[i] as u64) << 56 | (s[i + 1] as u64) << 48 | (s[i + 2] as u64) << 40 |
        (s[i + 3] as u64) << 32 | (s[i + 4] as u64) << 24 | (s[i + 5] as u64) << 16 |
        (s[i + 6] as u64) << 8 | s[i + 7]
}

// The bytes in another order of the leaves, at another integer type.
fn le64_reversed(s: const u8[:], i: i64) -> i64 {
    if i < 0 || i + 8 > s.len { return 0; }
    (s[i + 7] as i64) << 56 | (s[i + 6] as i64) << 48 | (s[i + 5] as i64) << 40 |
        (s[i + 4] as i64) << 32 | (s[i + 3] as i64) << 24 | (s[i + 2] as i64) << 16 |
        (s[i + 1] as i64) << 8 | s[i]
}
fn le32_shuffled(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 4 > s.len { return 0; }
    (s[i + 2] as u32) << 16 | s[i] | (s[i + 3] as u32) << 24 | (s[i + 1] as u32) << 8
}
fn be32_into_u64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i + 4 > s.len { return 0; }
    (s[i] as u64) << 24 | (s[i + 1] as u64) << 16 | (s[i + 2] as u64) << 8 | s[i + 3]
}
fn le16_into_u32(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 2 > s.len { return 0; }
    s[i] | (s[i + 1] as u32) << 8
}
// Shifted at a narrower type than the result's: each byte still whole.
fn be16_shifted_narrow(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i + 2 > s.len { return 0; }
    (s[i] as u16) << 8 | (s[i + 1] as u64)
}

// The span proven by a slice of its length, or an assert.
fn le64_sliced(s: const u8[:], i: i64) -> u64 {
    let c = s[i..i + 8];
    c[0] | (c[1] as u64) << 8 | (c[2] as u64) << 16 | (c[3] as u64) << 24 |
        (c[4] as u64) << 32 | (c[5] as u64) << 40 | (c[6] as u64) << 48 | (c[7] as u64) << 56
}
fn be32_sliced(s: const u8[:], i: i64) -> u32 {
    let c = s[i..i + 4];
    (c[0] as u32) << 24 | (c[1] as u32) << 16 | (c[2] as u32) << 8 | c[3]
}
fn le16_asserted(s: const u8[:], i: i64) -> u16 {
    assert(i >= 0 && i + 2 <= s.len);
    s[i] | (s[i + 1] as u16) << 8
}
// The last bytes, at offsets from the end.
fn be32_last(s: const u8[:]) -> u32 {
    if s.len < 4 { return 0; }
    (s[s.len - 4] as u32) << 24 | (s[s.len - 3] as u32) << 16 | (s[s.len - 2] as u32) << 8 |
        s[s.len - 1]
}
// A scaled index, the words of a block.
fn le32_word(s: const u8[:], j: i64) -> u32 {
    if j < 0 || 4 * j + 4 > s.len { return 0; }
    s[4 * j] | (s[4 * j + 1] as u32) << 8 | (s[4 * j + 2] as u32) << 16 | (s[4 * j + 3] as u32) << 24
}

// Fixed arrays: a local filled a byte at a time, read at constant and at
// proven offsets; by reference; by value; a field of a struct.
fn local_le64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i + 8 > s.len { return 0; }
    var r: u8[8] = [0; 8];
    for k in 8 { r[k] = s[i + k]; }
    r[0] | (r[1] as u64) << 8 | (r[2] as u64) << 16 | (r[3] as u64) << 24 |
        (r[4] as u64) << 32 | (r[5] as u64) << 40 | (r[6] as u64) << 48 | (r[7] as u64) << 56
}
fn local_be32_at(s: const u8[:], i: i64, j: i64) -> u32 {
    if i < 0 || i + 8 > s.len || j < 0 || j > 4 { return 0; }
    var r: u8[8] = [0; 8];
    for k in 8 { r[k] = s[i + k]; }
    (r[j] as u32) << 24 | (r[j + 1] as u32) << 16 | (r[j + 2] as u32) << 8 | r[j + 3]
}
fn ref_le32(a: u8[4]&) -> u32 {
    a[0] | (a[1] as u32) << 8 | (a[2] as u32) << 16 | (a[3] as u32) << 24
}
fn val_be16(a: u8[2]) -> u16 { (a[0] as u16) << 8 | a[1] }

struct Packet { tag: u8, body: u8[8] }

fn packet_be64(p: const Packet&) -> u64 {
    (p.body[0] as u64) << 56 | (p.body[1] as u64) << 48 | (p.body[2] as u64) << 40 |
        (p.body[3] as u64) << 32 | (p.body[4] as u64) << 24 | (p.body[5] as u64) << 16 |
        (p.body[6] as u64) << 8 | p.body[7]
}
// A resizable array's elements.
fn grown_le32(b: u8[>..]&, i: i64) -> u32 {
    if i < 0 || i + 4 > b.len { return 0; }
    b[i] | (b[i + 1] as u32) << 8 | (b[i + 2] as u32) << 16 | (b[i + 3] as u32) << 24
}

// Forms left byte by byte: a gap, the bytes out of either order, two arrays,
// an index whose u8 arithmetic wraps, three bytes, a check that is not
// proven away.
fn gap32(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 5 > s.len { return 0; }
    s[i] | (s[i + 1] as u32) << 8 | (s[i + 2] as u32) << 16 | (s[i + 4] as u32) << 24
}
fn swapped32(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 4 > s.len { return 0; }
    s[i] | (s[i + 1] as u32) << 16 | (s[i + 2] as u32) << 8 | (s[i + 3] as u32) << 24
}
fn two_arrays16(a: const u8[:], b: const u8[:]) -> u16 {
    if a.len < 1 || b.len < 2 { return 0; }
    a[0] | (b[1] as u16) << 8
}
fn wrapped16(s: const u8[:], b: u8) -> u16 {
    if s.len < 256 { return 0; }
    s[b] | (s[b + 1] as u16) << 8
}
fn le24(s: const u8[:], i: i64) -> u32 {
    if i < 0 || i + 3 > s.len { return 0; }
    s[i] | (s[i + 1] as u32) << 8 | (s[i + 2] as u32) << 16
}
fn unproven16(s: const u8[:], i: i64) -> u16 { s[i] | (s[i + 1] as u16) << 8 }

// The value of the n bytes from i in either order, a byte at a time.
fn ref_load(s: const u8[:], i: i64, n: i64, be: bool) -> u64 {
    if i < 0 || i + n > s.len { return 0; }
    var v: u64 = 0;
    for k in n {
        let at = if be { i + k } else { i + n - 1 - k };
        v = v * 256 + s[at];
    }
    v
}

// ---- stores

fn st_le16(o: u8[:], j: i64, v: u16) {
    if j < 0 || j + 2 > o.len { return; }
    for k in 2 { o[j + k] = (v >> (8 * k)) as! u8; }
}
fn st_be16(o: u8[:], j: i64, v: u16) {
    if j < 0 || j + 2 > o.len { return; }
    for k in 2 { o[j + k] = (v >> (8 - 8 * k)) as! u8; }
}
fn st_le32(o: u8[:], j: i64, v: u32) {
    if j < 0 || j + 4 > o.len { return; }
    for k in 4 { o[j + k] = (v >> (k * 8)) as! u8; }
}
fn st_be32(o: u8[:], j: i64, v: u32) {
    if j < 0 || j + 4 > o.len { return; }
    for k in 4 { o[j + k] = (v >> (8 * (3 - k))) as! u8; }
}
fn st_le64(o: u8[:], j: i64, v: u64) {
    if j < 0 || j + 8 > o.len { return; }
    for k in 0..8 { o[k + j] = ((v >> (8 * k)) & 255) as u8; }
}
fn st_be64(o: u8[:], j: i64, v: u64) {
    if j < 0 || j + 8 > o.len { return; }
    for k in 8 { o[j + k] = (v >> (56 - (k << 3))) as! u8; }
}
// The low 4 bytes of a wider value; a signed one.
fn st_le32_of_u64(o: u8[:], j: i64, v: u64) {
    if j < 0 || j + 4 > o.len { return; }
    for k in 4 { o[j + k] = (v >> (8 * k)) as! u8; }
}
fn st_be64_signed(o: u8[:], j: i64, v: i64) {
    if j < 0 || j + 8 > o.len { return; }
    for k in 8 { o[j + k] = (v >> (8 * (7 - k))) as! u8; }
}
// Runs of statements, in either order.
fn st_seq_le16(o: u8[:], j: i64, v: u16) {
    if j < 0 || j + 2 > o.len { return; }
    o[j] = v as! u8;
    o[j + 1] = (v >> 8) as! u8;
}
fn st_seq_be32(o: u8[:], j: i64, v: u32) {
    if j < 0 || j + 4 > o.len { return; }
    o[j] = (v >> 24) as! u8;
    o[j + 1] = (v >> 16) as! u8;
    o[j + 2] = (v >> 8) as! u8;
    o[j + 3] = v as! u8;
}
fn st_seq_le64_backward(o: u8[:], j: i64, v: u64) {
    if j < 0 || j + 8 > o.len { return; }
    o[j + 7] = ((v >> 56) & 255) as u8;
    o[j + 6] = ((v >> 48) & 255) as u8;
    o[j + 5] = ((v >> 40) & 255) as u8;
    o[j + 4] = ((v >> 32) & 255) as u8;
    o[j + 3] = ((v >> 24) & 255) as u8;
    o[j + 2] = ((v >> 16) & 255) as u8;
    o[j + 1] = ((v >> 8) & 255) as u8;
    o[j] = (v & 255) as u8;
}
fn st_seq_be64(o: u8[:], j: i64, v: u64) {
    if j < 0 || j + 8 > o.len { return; }
    o[j] = (v >> 56) as! u8;
    o[j + 1] = (v >> 48) as! u8;
    o[j + 2] = (v >> 40) as! u8;
    o[j + 3] = (v >> 32) as! u8;
    o[j + 4] = (v >> 24) as! u8;
    o[j + 5] = (v >> 16) as! u8;
    o[j + 6] = (v >> 8) as! u8;
    o[j + 7] = v as! u8;
}
// Into a local array, read back whole; into a resizable array.
fn swap_through_local(x: u64) -> u64 {
    var r: u8[8] = [0; 8];
    for k in 8 { r[k] = (x >> (8 * k)) as! u8; }
    (r[0] as u64) << 56 | (r[1] as u64) << 48 | (r[2] as u64) << 40 | (r[3] as u64) << 32 |
        (r[4] as u64) << 24 | (r[5] as u64) << 16 | (r[6] as u64) << 8 | r[7]
}
fn st_grown_be16(b: u8[>..]&, j: i64, v: u16) {
    if j < 0 || j + 2 > b.len { return; }
    b[j] = (v >> 8) as! u8;
    b[j + 1] = v as! u8;
}
// Left byte by byte: a byte out of place, a run broken by another store.
fn st_scrambled32(o: u8[:], j: i64, v: u32) {
    if j < 0 || j + 4 > o.len { return; }
    for k in 4 { o[j + k] = (v >> (8 * ((k + 1) % 4))) as! u8; }
}
fn st_interrupted16(o: u8[:], j: i64, v: u16, w: u8[:]) {
    if j < 0 || j + 2 > o.len || w.len < 1 { return; }
    o[j] = v as! u8;
    w[0] = 7;
    o[j + 1] = (v >> 8) as! u8;
}

// The n bytes of v from j in either order, a byte at a time.
fn ref_store(o: u8[:], j: i64, n: i64, v: u64, be: bool) {
    if j < 0 || j + n > o.len { return; }
    var x = v;
    for k in n {
        let at = if be { j + n - 1 - k } else { j + k };
        o[at] = (x % 256) as! u8;
        x = x / 256;
    }
}

// ---- the checks

// The bytes 0, 1, ... of a run of n, each distinct, none 0.
fn bytes(n: i64) -> u8[>..] {
    var b: u8[>..] = [];
    for k in n { b.push((k * 37 + 11) as! u8); }
    b
}

fn digest(s: const u8[:]) -> u64 {
    var h: u64 = 0;
    for x in s { h = h * 131 + x; }
    h
}

fn same(a: const u8[:], b: const u8[:]) -> bool {
    if a.len != b.len { return false; }
    for x, i in a {
        if x != b[i] { return false; }
    }
    true
}

var failures = 0;

fn check(what: const u8[:], n: i64, i: i64, got: u64, want: u64) {
    if got != want {
        failures += 1;
        print("FAIL ", what, " len ", n, " at ", i, ": ", got, " want ", want);
    }
}

// Each load at every offset of a slice of each length from 0 to 20, and
// one past either end, where the guard gives 0.
fn check_loads() {
    var h: u64 = 0;
    for n in 21 {
        let b = bytes(n);
        let s: const u8[:] = b;
        for i in -1..n + 1 {
            check("le16", n, i, le16(s, i), ref_load(s, i, 2, false));
            check("be16", n, i, be16(s, i), ref_load(s, i, 2, true));
            check("le32", n, i, le32(s, i), ref_load(s, i, 4, false));
            check("be32", n, i, be32(s, i), ref_load(s, i, 4, true));
            check("le64", n, i, le64(s, i), ref_load(s, i, 8, false));
            check("be64", n, i, be64(s, i), ref_load(s, i, 8, true));
            check("le64_reversed", n, i, le64_reversed(s, i) as! u64, ref_load(s, i, 8, false));
            check("le32_shuffled", n, i, le32_shuffled(s, i), ref_load(s, i, 4, false));
            check("be32_into_u64", n, i, be32_into_u64(s, i), ref_load(s, i, 4, true));
            check("le16_into_u32", n, i, le16_into_u32(s, i), ref_load(s, i, 2, false));
            check("be16_shifted_narrow", n, i, be16_shifted_narrow(s, i), ref_load(s, i, 2, true));
            check("le32_word", n, i, le32_word(s, i), ref_load(s, 4 * i, 4, false));
            check("local_le64", n, i, local_le64(s, i), ref_load(s, i, 8, false));
            check("gap32", n, i, gap32(s, i),
                  if i >= 0 && i + 5 <= n { ref_load(s, i, 3, false) + (s[i + 4] as u64) * 16777216 } else { 0 });
            check("le24", n, i, le24(s, i), ref_load(s, i, 3, false));
            for j in 5 {
                let want = if i >= 0 && i + 8 <= n { ref_load(s, i + j, 4, true) } else { 0 };
                check("local_be32_at", n, i * 8 + j, local_be32_at(s, i, j), want);
            }
            if i >= 0 && i + 8 <= n {
                check("le64_sliced", n, i, le64_sliced(s, i), ref_load(s, i, 8, false));
                check("be32_sliced", n, i, be32_sliced(s, i), ref_load(s, i, 4, true));
                check("le16_asserted", n, i, le16_asserted(s, i), ref_load(s, i, 2, false));
                check("unproven16", n, i, unproven16(s, i), ref_load(s, i, 2, false));
                let sw: u64 = s[i] | (s[i + 2] as u64) << 8 | (s[i + 1] as u64) << 16 |
                              (s[i + 3] as u64) << 24;
                check("swapped32", n, i, swapped32(s, i), sw);
            }
            h = h * 31 + le64(s, i) + be32(s, i) + le16(s, i);
        }
        check("be32_last", n, n, be32_last(s), ref_load(s, n - 4, 4, true));
    }
    for n in 21 {
        var g = bytes(n);
        for i in -1..n + 1 {
            let x = grown_le32(g, i);
            check("grown_le32", n, i, x, ref_load(g, i, 4, false));
        }
    }
    let s4: u8[4] = [0x11, 0x22, 0x33, 0x44];
    var a4 = s4;
    let s2: u8[2] = [0xAB, 0xCD];
    let p = Packet { tag: 1, body: [1, 2, 3, 4, 5, 6, 7, 8] };
    let big = bytes(300);
    let one: u8[1] = [9];
    print("loads ", h, " ", ref_le32(a4), " ", val_be16(s2), " ", packet_be64(p), " ",
          two_arrays16(one, big), " ", wrapped16(big, 255), " ", wrapped16(big, 7));
}

// Each store at every offset of a slice of each length from 0 to 20, and
// one past either end, where the guard stores nothing: what it leaves must
// be what a store a byte at a time leaves.
fn check_stores() {
    var h: u64 = 0;
    for n in 21 {
        var got = bytes(n);
        var want = bytes(n);
        for j in -1..n + 1 {
            let v = (j as! u64 + 1) * 0x0123456789ABCDEF;
            st_le16(got, j, v as! u16);
            ref_store(want, j, 2, v as! u16, false);
            st_be16(got, j + 1, v as! u16);
            ref_store(want, j + 1, 2, v as! u16, true);
            st_le32(got, j, v as! u32);
            ref_store(want, j, 4, v as! u32, false);
            st_be32(got, j - 1, v as! u32);
            ref_store(want, j - 1, 4, v as! u32, true);
            st_le64(got, j, v);
            ref_store(want, j, 8, v, false);
            st_be64(got, j + 2, v);
            ref_store(want, j + 2, 8, v, true);
            st_le32_of_u64(got, j, v);
            ref_store(want, j, 4, v as! u32, false);
            st_be64_signed(got, j, -(v as! i64));
            ref_store(want, j, 8, (-(v as! i64)) as! u64, true);
            st_seq_le16(got, j - 2, v as! u16);
            ref_store(want, j - 2, 2, v as! u16, false);
            st_seq_be32(got, j, v as! u32);
            ref_store(want, j, 4, v as! u32, true);
            st_seq_le64_backward(got, j, v);
            ref_store(want, j, 8, v, false);
            st_seq_be64(got, j - 3, v);
            ref_store(want, j - 3, 8, v, true);
            st_grown_be16(got, j, v as! u16);
            ref_store(want, j, 2, v as! u16, true);
            st_scrambled32(got, j, v as! u32);
            if j >= 0 && j + 4 <= n {
                for k in 4 { want[j + k] = ((v as! u32) >> (8 * ((k + 1) % 4))) as! u8; }
            }
            var w0: u8[1] = [0];
            st_interrupted16(got, j, v as! u16, w0);
            ref_store(want, j, 2, v as! u16, false);
            if !same(got, want) {
                failures += 1;
                print("FAIL stores len ", n, " at ", j, ": ", got, " want ", want);
            }
            h = h * 31 + digest(got);
        }
    }
    print("stores ", h, " ", swap_through_local(0x0102030405060708));
}

fn wide_bytes_run() {
    check_loads();
    check_stores();
    if failures == 0 {
        print("all wide loads and stores match");
    } else {
        print(failures, " FAILURES");
    }
}
