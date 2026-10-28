// goose-flags: -DGS_STACK_STATS=1
// Under GS_STACK_STATS each thread program reports its stacks on stderr as
// it ends, as C's gs_stack_stats does: a worker's by its id, with the cap
// on workers the budget gives.
var log: u8[>..] = [];

thread_fn worker(n: i64) {
    var s: u8[>..] = [];
    s.append("ab");
    log.push('x');
    qput(s.len + n + log.len);
}

fn main() {
    let id = thread_spawn(worker, 1);
    print(qget<i64>());
    thread_wait(id);
}
