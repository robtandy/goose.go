// The other of the two programs of one Go package (issue #19): see
// geom.goose. Its Pt, norm and calls are its own, which its file's name
// keeps apart from geom.goose's.
import std;
import dictionary;

// A position in a text: another Pt than geom.goose's.
struct Pt { line: i64, col: i64 }

var calls = 0;

fn norm(p: Pt) -> i64 { p.line * 1000 + p.col }

// How many different words text has, counted in a dictionary on the data
// stacks.
export fn distinct(text: const u8[:]) -> i64 {
    calls += 1;
    var d = dictionary<const u8[:], i32> {};
    each_split(text, ' ') {
        if it.len > 0 { d.update(it, 0) { it += 1; }; }
    };
    d.count
}

// Where text's last byte is: its line times 1000 and its column, from 1.
export fn last_pos(text: const u8[:]) -> i64 {
    var p = Pt { line: 1, col: 0 };
    for c in text {
        if c == '\n' {
            p.line += 1;
            p.col = 0;
        } else {
            p.col += 1;
        }
    }
    norm(p)
}

// How many times distinct ran.
export fn text_calls() -> i64 { calls }
