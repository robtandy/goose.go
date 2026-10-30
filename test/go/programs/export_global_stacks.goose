// export fns whose calls grow globals on data stacks of their own (issue
// #25), in a program with a main and no workers, which calls them as any
// function: the Go backend keeps the program's globals in program
// instances, main's here, as a program with workers does, so that each Go
// caller of the entry points runs on an instance of its block's own
// (test/go/export/arena/).
struct Pt { x: i64, y: i64 }

var arena: i64[>..] = [10, 20];
var pts: Pt[>..] = [];
var log: u8[>..] = "log:";
var calls = 0;
let SCALE: i64 = 3;

export fn push(x: i64) -> i64 {
    calls += 1;
    arena.push(x * SCALE);
    arena.len
}

export fn total() -> i64 {
    var s = 0;
    for x in arena { s += x; }
    s
}

fn fill(a: i64[>..]&, n: i64) {
    for i in n { a.push(i * i); }
}

export fn fill_arena(n: i64) -> i64 {
    fill(arena, n);
    arena.len
}

export fn add_pt(x: i64, y: i64) -> i64 {
    pts.push(Pt { x: x, y: y });
    pts.len
}

export fn last_pt() -> Pt {
    if pts.len == 0 { return Pt { x: -1, y: -1 }; }
    pts[pts.len - 1]
}

export fn note(text: const u8[:]) -> i64 {
    log.append(text);
    log.len
}

export fn last_log() -> const u8[:] { log[..] }

fn main() {
    print(push(1), " ", push(2), " ", total());
    print(fill_arena(4), " ", total(), " ", arena[..]);
    print(add_pt(1, 2), " ", add_pt(3, 4), " ", last_pt());
    print(note("a"), " ", note("bc"), " ", last_log());
    arena.clear();
    print(total(), " ", push(5), " ", calls);
}
