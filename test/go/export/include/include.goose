// extern fns that the Go of an --include file defines (issue #5), called
// directly: scalars, a slice as the generated Go's GsSlice, a reference as
// a pointer to the generated type, and a builder, which the function is
// handed after the data stack block to append to it with (GsBldAppend).
// export fns over them, which include_test.go calls.
extern fn mix(h: u64, b: u64) -> u64;
extern fn greet(name: const u8[:], out: u8[>..]&) -> bool;
extern fn clamp_to(x: f64, lo: f64, hi: f64) -> f64 = "goClamp";

struct Pt { x: i32, y: i32 }
extern fn manhattan(p: const Pt&) -> i64;

export fn hash_bytes(data: const u8[:]) -> u64 {
    var h: u64 = 1469598103934665603;
    for b in data { h = mix(h, b as u64); }
    h
}

// The greeting's length, built in a builder the Go appends to, between <>.
export fn greeting_len(name: const u8[:]) -> i64 {
    var out: u8[>..] = [];
    out.append("<");
    if !greet(name, out) { return -1; }
    out.append(">");
    out.len
}

// The greeting, copied into buf as far as it fits; its length.
export fn greeting(name: const u8[:], buf: u8[:]) -> i64 {
    var out: u8[>..] = [];
    if !greet(name, out) { return -1; }
    for c, i in out {
        if i < buf.len { buf[i] = c; }
    }
    out.len
}

export fn clamped(x: f64) -> f64 { clamp_to(x, 0.0, 1.0) }

export fn dist(x: i32, y: i32) -> i64 {
    let p = Pt { x: x, y: y };
    manhattan(p)
}
