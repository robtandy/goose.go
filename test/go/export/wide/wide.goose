// Wide loads and stores (issue #23), through the Go entry points of export
// fns: wide_test.go compares each with encoding/binary at every offset of
// slices of every length up to 24, the last bytes included, and
// run_go_tests.py looks at the code Go makes of each annotated one for arm64
// and for amd64 (Runner.wide_asm): `asm: load n` is one memory access, an
// n-byte load, `asm: store n` one n-byte store, and `asm: none` none through
// a pointer at all, the function's own stack aside. An offset whose bytes
// are not all in the slice reads as 1 << 63 and writes nothing.

// ---- loads

// asm: load 2
export fn le16(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 2 { return 0x8000000000000000; }
    s[i] | (s[i + 1] as u64) << 8
}
// asm: load 2
export fn be16(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 2 { return 0x8000000000000000; }
    (s[i] as u64) << 8 | s[i + 1]
}
// asm: load 4
export fn le32(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 4 { return 0x8000000000000000; }
    s[i] | (s[i + 1] as u64) << 8 | (s[i + 2] as u64) << 16 | (s[i + 3] as u64) << 24
}
// asm: load 4
export fn be32(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 4 { return 0x8000000000000000; }
    (s[i] as u64) << 24 | (s[i + 1] as u64) << 16 | (s[i + 2] as u64) << 8 | s[i + 3]
}
// asm: load 8
export fn le64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 8 { return 0x8000000000000000; }
    s[i] | (s[i + 1] as u64) << 8 | (s[i + 2] as u64) << 16 | (s[i + 3] as u64) << 24 |
        (s[i + 4] as u64) << 32 | (s[i + 5] as u64) << 40 | (s[i + 6] as u64) << 48 |
        (s[i + 7] as u64) << 56
}
// asm: load 8
export fn be64(s: const u8[:], i: i64) -> u64 {
    if i < 0 || i > s.len - 8 { return 0x8000000000000000; }
    (s[i] as u64) << 56 | (s[i + 1] as u64) << 48 | (s[i + 2] as u64) << 40 |
        (s[i + 3] as u64) << 32 | (s[i + 4] as u64) << 24 | (s[i + 5] as u64) << 16 |
        (s[i + 6] as u64) << 8 | s[i + 7]
}
// The first 8 bytes of a slice at least that long, at constant offsets from
// its start, the first through the slice's own pointer.
// asm: load 8
export fn le64_head(s: const u8[:]) -> u64 {
    if s.len < 8 { return 0x8000000000000000; }
    s[0] | (s[1] as u64) << 8 | (s[2] as u64) << 16 | (s[3] as u64) << 24 |
        (s[4] as u64) << 32 | (s[5] as u64) << 40 | (s[6] as u64) << 48 | (s[7] as u64) << 56
}
// The last 4.
// asm: load 4
export fn be32_tail(s: const u8[:]) -> u64 {
    if s.len < 4 { return 0x8000000000000000; }
    (s[s.len - 4] as u64) << 24 | (s[s.len - 3] as u64) << 16 | (s[s.len - 2] as u64) << 8 |
        s[s.len - 1]
}
// A fixed array by reference.
// asm: load 8
export fn be64_of(a: const u8[8]&) -> u64 {
    (a[0] as u64) << 56 | (a[1] as u64) << 48 | (a[2] as u64) << 40 | (a[3] as u64) << 32 |
        (a[4] as u64) << 24 | (a[5] as u64) << 16 | (a[6] as u64) << 8 | a[7]
}

// ---- stores, which report whether the bytes were in the slice

// asm: store 2
export fn put_le16(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 2 { return false; }
    for k in 2 { o[j + k] = (v >> (8 * k)) as! u8; }
    true
}
// asm: store 2
export fn put_be16(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 2 { return false; }
    for k in 2 { o[j + k] = (v >> (8 * (1 - k))) as! u8; }
    true
}
// asm: store 4
export fn put_le32(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 4 { return false; }
    for k in 4 { o[j + k] = (v >> (8 * k)) as! u8; }
    true
}
// asm: store 4
export fn put_be32(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 4 { return false; }
    for k in 4 { o[j + k] = (v >> (8 * (3 - k))) as! u8; }
    true
}
// asm: store 8
export fn put_le64(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 8 { return false; }
    for k in 8 { o[j + k] = (v >> (8 * k)) as! u8; }
    true
}
// asm: store 8
export fn put_be64(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 8 { return false; }
    for k in 8 { o[j + k] = (v >> (8 * (7 - k))) as! u8; }
    true
}
// The same as runs of statements.
// asm: store 4
export fn put_le32_unrolled(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 4 { return false; }
    o[j] = v as! u8;
    o[j + 1] = (v >> 8) as! u8;
    o[j + 2] = (v >> 16) as! u8;
    o[j + 3] = (v >> 24) as! u8;
    true
}
// asm: store 8
export fn put_be64_unrolled(o: u8[:], j: i64, v: u64) -> bool {
    if j < 0 || j > o.len - 8 { return false; }
    o[j] = (v >> 56) as! u8;
    o[j + 1] = (v >> 48) as! u8;
    o[j + 2] = (v >> 40) as! u8;
    o[j + 3] = (v >> 32) as! u8;
    o[j + 4] = (v >> 24) as! u8;
    o[j + 5] = (v >> 16) as! u8;
    o[j + 6] = (v >> 8) as! u8;
    o[j + 7] = v as! u8;
    true
}

// ---- a local fixed array, written a byte at a time and read back whole:
// a Go value on Go's stack, never the fixed-frame stack's.

// asm: none
export fn swap64(x: u64) -> u64 {
    var r: u8[8] = [0; 8];
    for k in 8 { r[k] = (x >> (8 * k)) as! u8; }
    (r[0] as u64) << 56 | (r[1] as u64) << 48 | (r[2] as u64) << 40 | (r[3] as u64) << 32 |
        (r[4] as u64) << 24 | (r[5] as u64) << 16 | (r[6] as u64) << 8 | r[7]
}
// The 8 bytes from i copied a byte at a time into a local array, then read
// back whole, from an offset in it.
export fn le32_via_local(s: const u8[:], i: i64, j: i64) -> u64 {
    if i < 0 || i > s.len - 8 || j < 0 || j > 4 { return 0x8000000000000000; }
    var r: u8[8] = [0; 8];
    for k in 8 { r[k] = s[i + k]; }
    r[j] | (r[j + 1] as u64) << 8 | (r[j + 2] as u64) << 16 | (r[j + 3] as u64) << 24
}

// ---- for go test -bench: the sum of the little-endian words of s, 8 bytes
// at a time, one load each; and a byte at a time, `+` where `|` would be,
// of which no load is made.

// asm: load 8
export fn sum_le64(s: const u8[:]) -> u64 {
    var h: u64 = 0;
    var i = 0;
    while i <= s.len - 8 {
        h += s[i] | (s[i + 1] as u64) << 8 | (s[i + 2] as u64) << 16 | (s[i + 3] as u64) << 24 |
            (s[i + 4] as u64) << 32 | (s[i + 5] as u64) << 40 | (s[i + 6] as u64) << 48 |
            (s[i + 7] as u64) << 56;
        i += 8;
    }
    h
}
export fn sum_le64_bytewise(s: const u8[:]) -> u64 {
    var h: u64 = 0;
    var i = 0;
    while i <= s.len - 8 {
        h += s[i] + ((s[i + 1] as u64) << 8) + ((s[i + 2] as u64) << 16) + ((s[i + 3] as u64) << 24) +
            ((s[i + 4] as u64) << 32) + ((s[i + 5] as u64) << 40) + ((s[i + 6] as u64) << 48) +
            ((s[i + 7] as u64) << 56);
        i += 8;
    }
    h
}
