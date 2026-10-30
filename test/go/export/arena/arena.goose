// A library whose export fns grow globals that live on data stacks of their
// own (issue #25), compiled with --target=go --library: arena_test.go, in
// the package the Go is, calls their Go entry points from many goroutines
// at once. The program spawns no workers, and yet each call runs on a
// program instance of its block's own (goose_spec.md §11.1: a global's
// storage belongs to the program instance running it), as in a program
// with workers: a block a caller keeps keeps its globals across the
// caller's calls, and a call on a block from the runtime's pool starts from
// the package's, so that no two goroutines share a global. The file holds
// none of the workers' and queues' runtime (run_go_tests.py checks it).

// The package's: what the library's start leaves in them is what every
// instance starts from.
var arena: i64[>..] = [1, 2, 3];
var log: u8[>..] = "init:";
var pushes = 0;
// The text the last note appended, a view of the log: the start leaves it
// viewing static data, so that the copy an instance starts from views
// nothing of the package's, and each views its own log once it notes.
var newest: const u8[:] = "";

// Pushes x onto this instance's arena, a growable arena on a data stack of
// its own (its block's): the arena's length after.
export fn push(x: i64) -> i64 {
    pushes += 1;
    arena.push(x);
    arena.len
}

// The sum of this instance's arena.
export fn total() -> i64 {
    var s = 0;
    for x in arena { s += x; }
    s
}

// Element i of this instance's arena, or -1 where it has none.
export fn element(i: i64) -> i64 {
    if i < 0 || i >= arena.len { return -1; }
    arena[i]
}

// Pushes 0, 1, ..., n - 1 onto the array a refers to.
fn fill(a: i64[>..]&, n: i64) {
    for i in n { a.push(i); }
}

// Fills this instance's arena with n more, handing on a reference to it:
// its length after.
export fn fill_arena(n: i64) -> i64 {
    fill(arena, n);
    arena.len
}

// Empties this instance's arena.
export fn clear_arena() { arena.clear(); }

// Appends text to this instance's log, a global on a data stack of its own
// too: the log's length after.
export fn note(text: const u8[:]) -> i64 {
    let at = log.len;
    log.append(text);
    newest = log[at..];
    log.len
}

// The text this instance's last note appended.
export fn newest_note() -> const u8[:] { newest }

// The log as it is in this instance: a view of it, which a pooled call
// hands back as a copy, its block gone back to the pool.
export fn last_log() -> const u8[:] { log[..] }

// How many pushes this instance has seen.
export fn push_count() -> i64 { pushes }
