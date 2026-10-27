// export fns (issue #5) in a program with a main, which calls them as any
// function: the Go backend gives each Go entry points as well, Name and
// NameWithStack, which a Go caller of the package uses (test/go/export/).
struct Rect { x: f64, y: f64, w: f64, h: f64 }

export fn area(r: const Rect&) -> f64 { r.w * r.h }

export fn scaled(r: Rect, k: f64) -> Rect { Rect { x: r.x, y: r.y, w: r.w * k, h: r.h * k } }

// The bytes of text that are not spaces, on a data stack, and their sum.
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

export fn note() { print("noted"); }

fn main() {
    let r = Rect { x: 1.0, y: 2.0, w: 3.0, h: 4.0 };
    let half = scaled(r, 0.5);
    print(area(r), " ", scaled(r, 2.0), " ", area(half));
    print(checksum("a b c"), " ", checksum(""), " ", checksum("goose go"));
    var buf: u8[8] = "Mixed Up";
    print(upcase(buf), " ", buf[..]);
    print(first_word("hello world"), "|", first_word("single"), "|", first_word(""));
    print(total([1, 2, 3, 4]), " ", total([]));
    note();
}
