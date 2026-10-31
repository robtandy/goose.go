// A library whose export fns grow the components of root globals (issue
// #59), each on a data stack of its own, compiled with --target=go
// --library: root_globals_test.go calls their Go entry points from many
// goroutines at once. As for any global on a data stack (issue #25), each
// call runs on a program instance of its block's own, which copies each
// component the export's calls reach and makes the owner's descriptor
// again, of references to the copies: a block a caller keeps keeps the
// owners across its calls, and a call on a block from the runtime's pool
// starts from the package's.
import slot_pool;

// An owner the program declares, with two growable arrays and a count.
root struct Log { lines: u8[>..] = [], marks: i64[>..<] = [], count: i64 = 0 }

// The package's: what the library's start leaves in them is what every
// instance starts from.
var log = Log { "start;", [], 1 };
var pool = slot_pool<i64> {};

// A note in the log an owner's reference names, as a callee that stays a
// call sees it: through its descriptor.
fn note(l: Log&, text: const u8[:]) {
    l.lines.append(text);
    l.marks.push(l.lines.len);
    l.count += 1;
}

// Appends text to this instance's log: how many notes it holds after.
export fn add(text: const u8[:]) -> i64 {
    note(log, text);
    log.count
}

// The bytes of this instance's log.
export fn lines() -> const u8[:] { log.lines[..] }

// Where the last note of this instance's log ends, -1 where it has none.
export fn last_mark() -> i64 {
    if log.marks.len == 0 { return -1; }
    log.marks[log.marks.len - 1]
}

// A slot of this instance's pool holding v: its index.
export fn take(v: i64) -> i64 { pool.alloc_index(v) }

// Slot i of this instance's pool back on its freelist.
export fn give(i: i64) { pool.free(i); }

// What slot i of this instance's pool holds, -1 where it has none.
export fn slot(i: i64) -> i64 {
    if i < 0 || i >= pool.items.len { return -1; }
    pool.items[i]
}
