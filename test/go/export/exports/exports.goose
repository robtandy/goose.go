// A library of export fns (issue #5), compiled with --target=go --library:
// exports_test.go, in the package the Go is, calls their Go entry points,
// checks what they give, and that a call allocates nothing on Go's heap.
import std;
import dictionary;
import json;

struct Rect { x: f64, y: f64, w: f64, h: f64 }

// The area of a rectangle.
export fn area(r: const Rect&) -> f64 { r.w * r.h }

export fn scaled(r: Rect, k: f64) -> Rect { Rect { x: r.x, y: r.y, w: r.w * k, h: r.h * k } }

export fn twice(x: i64) -> i64 { x * 2 }

// The bytes of text that are not spaces, kept on a data stack, each
// weighted by its place among them.
export fn checksum(text: const u8[:]) -> i64 {
    var kept: u8[>..] = [];
    for c in text {
        if c != ' ' { kept.push(c); }
    }
    var sum = 0;
    for c, i in kept {
        let v: i64 = c;
        sum += (i + 1) * v;
    }
    sum
}

// Upper-cases buf in place; how many bytes it changed.
export fn upcase(buf: u8[:]) -> i64 {
    var n = 0;
    for &c in buf {
        if c >= 'a' && c <= 'z' {
            c -= 32;
            n++;
        }
    }
    n
}

// The text up to its first space: a view of it.
export "first_word_of" fn first_word(text: const u8[:]) -> const u8[:] {
    for c, i in text {
        if c == ' ' { return text[..i]; }
    }
    text
}

export fn total(xs: const i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    s
}

// How many different words text has, counted in a dictionary on the data
// stacks.
export fn distinct_words(text: const u8[:]) -> i64 {
    var d = dictionary<const u8[:], i32> {};
    each_split(text, ' ') {
        if it.len > 0 { d.update(it, 0) { it += 1; }; }
    };
    d.count
}

// The sum of the numbers of a JSON document, parsed onto the data stacks;
// -1 where it is no JSON.
export fn json_sum(text: const u8[:]) -> f64 {
    var doc: json::Node[>..] = [];
    var decoded: u8[>..] = [];
    let root, err = json::parse(text, doc, decoded);
    guard root else { return -1.0; }
    var sum = 0.0;
    for &n in doc {
        match n.val {
            Num x => { sum += x.v; },
            Int i => { sum += i.v; },
            _ => {},
        }
    }
    sum
}

// An abort crosses to the Go caller as a panic.
export fn checked_div(a: i64, b: i64) -> i64 {
    if b == 0 { abort(str("checked_div: ", a, " by zero")); }
    a / b
}

// A global the package keeps between calls.
var calls: i64 = 0;
export fn count_call() -> i64 {
    calls += 1;
    calls
}
