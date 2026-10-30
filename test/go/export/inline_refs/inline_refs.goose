// By-reference parameters of inlined functions (issue #24), through the Go
// entry points of export fns: inline_refs_test.go checks what each computes
// against Go written by hand, and run_go_tests.py reads the Go of each
// annotated one at -O2, where the helpers are inlined (Runner.frames):
// `frame: none` is a function that takes no frame on the fixed-frame stack
// (gs.Frame, DESIGN.md §3.6), every local it hands an inlined body by
// reference staying a Go local, which Go keeps in a register; `frame: n`, one
// whose frame holds n slots, one per local whose address does outlive the
// body that took it by reference.

// ---- the issue's sample

inline fn bump(x: i64&) { x += 1; }

// frame: none
export fn bumps(n: i64) -> i64 {
    var c = 0;
    for i in n { bump(c); }
    c
}

// ---- a parser-shaped loop: its state is six scalars, which one inlined
// helper takes by reference, a byte at a time

// One byte of a scan over JSON-like text: a number's digits, a string up to
// its closing quote, and the brackets of containers, which count the values
// and the depth; the value the byte ends, folded, or 0. scanGo in
// inline_refs_test.go is the same in Go.
inline fn step(text: const u8[:], tag: u64&, uni: u64&, cur: i64&, ctn: i64&, count: i64&,
               in_obj: bool&) -> u64 {
    let c = text[cur];
    cur += 1;
    if tag == 2 {
        if c == '"' {
            tag = 0;
            count += 1;
            return uni;
        }
        uni = uni * 31 + c;
        return 0;
    }
    if c >= '0' && c <= '9' {
        if tag != 1 {
            tag = 1;
            uni = 0;
        }
        uni = uni * 10 + (c - '0');
        return 0;
    }
    var done: u64 = 0;
    if tag == 1 {
        tag = 0;
        count += 1;
        done = uni;
    }
    if c == '"' {
        tag = 2;
        uni = 7;
    } else if c == '{' || c == '[' {
        ctn += 1;
        in_obj = c == '{';
    } else if c == '}' || c == ']' {
        ctn -= 1;
        count += 1;
        done += ctn as u64 * 3 + c;
    }
    done
}

// frame: none
export fn scan(text: const u8[:]) -> u64 {
    var tag: u64 = 0;
    var uni: u64 = 0;
    var cur = 0;
    var ctn = 0;
    var count = 0;
    var in_obj = false;
    var sum: u64 = 0;
    while cur < text.len {
        sum = sum * 31 + step(text, tag, uni, cur, ctn, count, in_obj);
    }
    sum ^ (count as u64) << 32 ^ (ctn as u64) << 16 ^ tag ^ (if in_obj { 1 } else { 0 })
}

// ---- places reached through references, and references handed on

// A reference handed on to another inlined body.
inline fn bump2(x: i64&) {
    bump(x);
    x = x * 2 + 1;
}

struct Pt { x: i64, y: i64 }

// A struct by reference, a field of it handed on.
inline fn nudge(p: Pt&) {
    p.x += 3;
    bump(p.y);
}

// A fixed array by reference, indexed.
inline fn mark(a: u8[8]&, i: i64) {
    a[i] += 1;
    a[0] = a[i] + 1;
}

// Two references to one place.
inline fn twice(x: i64&, y: i64&) {
    x += 1;
    y += 10;
    x += y;
}

// The place a reference names is the one it named when it was bound,
// whatever the index that named it holds after.
inline fn shift(x: i64&, k: i64&) {
    k += 1;
    x += k;
}

// frame: none
export fn places(n: i64) -> i64 {
    var c = 0;
    var p = Pt { x: 1, y: 2 };
    var a: u8[8] = [0, 0, 0, 0, 0, 0, 0, 0];
    var e: i64[4] = [1, 2, 3, 4];
    var k = 0;
    var d = 1;
    for i in n {
        bump2(c);
        nudge(p);
        mark(a, i & 7);
        bump(e[i & 3]);
        bump(p.x);
        twice(d, d);
        d = d & 1023;
        k = k & 3;
        shift(e[k], k);
    }
    c + p.x * 10 + p.y * 100 + (a[0] + a[3]) as i64 * 1000 + e[1] * 10000 + e[2] * 100000 +
        d * 1000000
}

// ---- references that outlive the body: their locals alone take the frame

// Goose inlines no function of several results.
fn keep(x: i64&) -> i64, bool {
    x += 1;
    return x, true;
}

// x is handed to a call, y only used here.
inline fn keep_one(x: i64&, y: i64&) {
    y += 1;
    let k, ok = keep(x);
    if ok { y += k; }
}

// frame: 1
export fn kept(n: i64) -> i64 {
    var a = 0;
    var b = 0;
    for i in n { keep_one(a, b); }
    a * 1000000 + b
}

// The reference is the body's value, and lives on in r.
inline fn larger(a: i64&, b: i64&) -> i64& {
    if a > b { a } else { b }
}

// frame: 2
export fn largest(n: i64) -> i64 {
    var a = 0;
    var b = 1;
    var c = 0;
    for i in n {
        let r: i64& = larger(a, b);
        r += i;
        bump(c);
    }
    a * 1000000 + b * 10 + c
}
