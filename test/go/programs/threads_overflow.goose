// goose-flags: --stack-reserve 64K
// A worker's data stack that grows past its region ends the program with
// the report C's fault handler gives, which names the worker.
thread_fn grow(n: i64) {
    var xs: i64[>..] = [];
    for i in n { xs.push(i); }
    print(xs.len);
}

fn main() {
    print("start");
    thread_wait(thread_spawn(grow, 100));
    thread_wait(thread_spawn(grow, 100000000));
    print("not reached");
}
