// export fns growing the components of root globals (issue #59) in a
// program with a main, which calls them as any function: as for any global
// on a data stack (issue #25), the program's globals are in program
// instances, main's here, so that each Go caller of the entry points runs
// on a copy of its own, the owners' descriptors made again for it
// (test/go/export/root_globals/).
import slot_pool;

root struct Log { lines: u8[>..] = [], marks: i64[>..<] = [], count: i64 = 0 }

var log = Log { "start;", [], 1 };
var pool = slot_pool<i64> {};

fn note(l: Log&, text: const u8[:]) {
    l.lines.append(text);
    l.marks.push(l.lines.len);
    l.count += 1;
}

export fn add(text: const u8[:]) -> i64 {
    note(log, text);
    log.count
}

export fn take(v: i64) -> i64 { pool.alloc_index(v) }
export fn give(i: i64) { pool.free(i); }

fn main() {
    print(add("ab"), " ", add("cde"), " ", log.lines, " ", log.marks);
    let a = take(5);
    let b = take(6);
    give(a);
    print(a, " ", b, " ", take(7), " ", pool.items, " ", pool.free_slots.len);
}
