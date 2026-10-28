// Workers whose programs return from afar (§7.9) while main's does: each
// thread program has the channels of its own, in its program instance.
recursive fn dive(depth: i64, at: i64) {
    if depth == at { return depth * 100 from search; }
    dive(depth + 1, at);
}
fn search(at: i64) -> i64 {
    dive(0, at);
    return -1;
}

recursive fn grow(depth: i64, k: i64) {
    if depth == k {
        var out: u8[>..] = [];
        for i in k { out.push(('a' + i) as u8); }
        return out from collect;
    }
    grow(depth + 1, k);
}
fn collect(k: i64) -> u8[>..] {
    var mine: u8[>..] = [];
    mine.append("never");
    grow(0, k);
    return mine;
}

thread_fn worker(k: i64) {
    var acc = 0;
    for r in 3000 { acc += search(k % 7); }
    let s = collect(k % 5 + 1);
    qput(acc + s.len);
}

fn main() {
    var ids: i64[>..] = [];
    for k in 6 { ids.push(thread_spawn(worker, k + 5)); }
    var total = 0;
    for r in 3000 { total += search(3); }
    for k in 6 { total += qget<i64>(); }
    for id in ids { thread_wait(id); }
    print(total, " ", collect(4));
}
