// export fns that keep what their Go caller hands them only where Go's
// collector does not look -- on a data stack, or in a struct, whose Go value
// is its bytes -- and call back into Go (collect, helper.go), which runs the
// collector, before they read it again (issue #30). Go keeps an argument
// alive only up to its last use, which may be the call itself, so an entry
// point keeps every argument it hands on alive until the call returns:
// keepalive_test.go hands each one an argument as the argument's last use,
// and checks that the collector freed none of them during the call.
extern fn collect();

struct Pt { x: i64, y: i64 }

// The view of xs, in a struct: its Go value is 24 bytes, which hold xs's
// address where the collector does not see it.
struct Held { n: i64, xs: const i64[:] }

// A slice, kept on a data stack.
export fn kept_sum(xs: const i64[:]) -> i64 {
    var kept: (const i64[:])[>..] = [];
    kept.push(xs);
    collect();
    var s = 0;
    for x in kept[0] { s += x; }
    s
}

// The bytes of a string or a []byte, kept on a data stack.
export fn kept_bytes(text: const u8[:]) -> i64 {
    var kept: (const u8[:])[>..] = [];
    kept.push(text);
    collect();
    var s = 0;
    for c in kept[0] { s += c; }
    s
}

// A []byte it may write, kept on a data stack.
export fn kept_count(buf: u8[:], b: u8) -> i64 {
    var kept: (u8[:])[>..] = [];
    kept.push(buf);
    collect();
    var n = 0;
    for c in kept[0] {
        if c == b { n += 1; }
    }
    n
}

// Fixed-size elements, kept on a data stack.
export fn kept_pts(ps: const Pt[:]) -> i64 {
    var kept: (const Pt[:])[>..] = [];
    kept.push(ps);
    collect();
    var s = 0;
    for p in kept[0] { s += p.x * p.y; }
    s
}

// A reference, kept on a data stack.
export fn kept_pt(p: const Pt&) -> i64 {
    var kept: (const Pt&)[>..] = [];
    kept.push(p);
    collect();
    kept[0].x - kept[0].y
}

// A Go []string (issue #42), kept on a data stack: the slice of the strings,
// and each string.
export fn kept_tags(tags: const (const u8[:])[:]) -> i64 {
    var kept: (const (const u8[:])[:])[>..] = [];
    kept.push(tags);
    var each: (const u8[:])[>..] = [];
    for t in tags { each.push(t); }
    collect();
    var s = 0;
    for t in kept[0] {
        for c in t { s += c; }
    }
    for t in each { s += t.len; }
    s
}

// A slice, kept in a struct.
export fn held_sum(xs: const i64[:]) -> i64 {
    let h = Held { n: 1, xs: xs };
    collect();
    var s = h.n;
    for x in h.xs { s += x; }
    s
}
