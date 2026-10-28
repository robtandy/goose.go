// A library of export fns in a program with workers (issue #12), compiled
// with --target=go --library: workers_test.go, in the package the Go is,
// calls their Go entry points from many goroutines at once. Each call runs
// on a program instance of its block's own (goose_spec.md §11.2): a block
// a caller keeps keeps its globals across the caller's calls, and a call on
// a block from the runtime's pool starts from the package's, so that no
// two goroutines share a global, as no two Goose threads do.

// The package's: what init leaves in them is what every instance starts
// from.
var calls: i64 = 0;
var log: u8[>..] = "init:";
let SQUARES: i64[8] = [0, 1, 4, 9, 16, 25, 36, 49];

// A worker's program has its own copies, taken from the spawning call's
// instance: it sees what the call wrote before the spawn, and what it
// writes stays its own.
thread_fn check(want: i64, wantlog: i64) {
    assert(calls == want && log.len == wantlog);
    calls += 1000;
    log.append("worker");
}

// Counts k in this instance's `calls`, then checks that a worker spawned
// from it starts from the same: the count, or -1 where the worker's copy
// differed (its assert aborts it, which is contained).
export fn bump(k: i64) -> i64 {
    calls += k;
    if !thread_wait(thread_spawn_contained(check, calls, log.len)) { return -1; }
    calls
}

// Appends text to this instance's log, a global on a data stack of its own
// (its block's): the log's length after.
export fn note(text: const u8[:]) -> i64 {
    log.append(text);
    log.len
}

// The log as it is in this instance: a view of it, which a pooled call
// hands back as a copy, its block gone back to the pool.
export fn last_log() -> const u8[:] { log[..] }

// A sum computed by workers, each its own part, collected through a queue:
// the queues are the process's (§11.2), shared by every instance, so this
// one is called from one goroutine at a time.
struct Part { from: i64, sum: i64 }
thread_fn part(from: i64, n: i64) {
    var s = 0;
    for i in n { s += SQUARES[(from + i) % 8] + calls; }
    qput(Part { from: from, sum: s });
}

export fn parallel_sum(workers: i64) -> i64 {
    var ids: i64[>..] = [];
    for w in workers { ids.push(thread_spawn(part, w * 10, 10)); }
    var total = 0;
    for w in workers { total += qget<Part>().sum; }
    for id in ids { thread_wait(id); }
    total
}
