// One of two programs compiled into one Go package (issue #19), which share
// its runtime (--go-runtime-file goose_runtime.go): text.goose is the other.
// Both declare a struct Pt, a function norm and a global calls, which their
// files' names keep apart in the package (Pt_geom_g, Pt_text_g); their
// export fns' entry points are the package's. shared_test.go calls them all.
import std;

struct Pt { x: i64, y: i64 }

var calls = 0;

fn norm(p: Pt) -> i64 { p.x * p.x + p.y * p.y }

// The sum of the squared lengths of the steps of the path through the
// points xs holds, an x and a y each, kept on a data stack.
export fn path_len2(xs: const i64[:]) -> i64 {
    calls += 1;
    var pts: Pt[>..] = [];
    var i = 0;
    while i + 1 < xs.len {
        pts.push(Pt { x: xs[i], y: xs[i + 1] });
        i += 2;
    }
    var s = 0;
    var prev = Pt { x: 0, y: 0 };
    for p, k in pts {
        if k > 0 { s += norm(Pt { x: p.x - prev.x, y: p.y - prev.y }); }
        prev = p;
    }
    s
}

// The length of a point's text.
export fn pt_text_len(x: i64, y: i64) -> i64 { str(Pt { x: x, y: y }).len }

// How many times path_len2 ran.
export fn geom_calls() -> i64 { calls }

// a over b, which aborts for a zero b.
export fn checked_div(a: i64, b: i64) -> i64 {
    if b == 0 { abort(str("checked_div: ", a, " by zero")); }
    a / b
}
