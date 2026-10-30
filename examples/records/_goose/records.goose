// The kernel of package records (docs/go-integration.md): countRecordsGo of
// count.go, ported to Goose and checksumming 8 bytes at a time. go generate
// -tags goose compiles it to records_goose.go, as bind_goose.go says.

// The little-endian u32 at s[i..i + 4], in one load. The slice checks the
// span, which fails where s ends inside it.
fn le32(s: const u8[:], i: i64) -> u32 {
    let c = s[i..i + 4];
    c[0] | (c[1] as u32) << 8 | (c[2] as u32) << 16 | (c[3] as u32) << 24
}

// The little-endian u64 at s[i..i + 8], in one load.
fn le64(s: const u8[:], i: i64) -> u64 {
    let c = s[i..i + 8];
    c[0] | (c[1] as u64) << 8 | (c[2] as u64) << 16 | (c[3] as u64) << 24 |
        (c[4] as u64) << 32 | (c[5] as u64) << 40 | (c[6] as u64) << 48 | (c[7] as u64) << 56
}

// The XOR of p's bytes: 8 at a time, then those of the tail, then the 8
// lanes folded into one.
fn xor_bytes(p: const u8[:]) -> u8 {
    var x: u64 = 0;
    var i = 0;
    while i <= p.len - 8 {
        x ^= le64(p, i);
        i += 8;
    }
    while i < p.len {
        x ^= p[i];
        i += 1;
    }
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x as! u8
}

// The number of records in seg, or -1 where one's checksum is wrong. A
// record is a little-endian u32 length n, n bytes of payload, and the XOR of
// those bytes. A record that seg ends inside fails a bounds check.
export fn count_records(seg: const u8[:]) -> i64 {
    var n = 0;
    var off = 0;
    while off < seg.len {
        let size = le32(seg, off) as i64;
        let p = seg[off + 4..off + 4 + size];
        if xor_bytes(p) != seg[off + 4 + size] { return -1; }
        off += 5 + size;
        n += 1;
    }
    n
}
