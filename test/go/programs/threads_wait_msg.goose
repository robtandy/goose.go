// goose-flags: --debug
// thread_wait(id, msg) appends a contained worker's abort message to a
// limited array as well as to a growable one, after what it holds, under
// its capacity check; the first wait takes the message.
thread_fn fail(kind: i64) {
    let xs = [1, 2, 3];
    if kind == 0 { abort("plain"); }
    if kind == 1 { print(xs[kind + 4]); }
    if kind == 2 { let big = (kind * 2000000000) as i32; print(big); }
}

fn located(msg: u8[:]) -> i64 {
    var i = msg.len;
    while i > 0 && msg[i - 1] != '(' { i--; }
    if i < 2 { return msg.len; }
    i - 2
}

fn main() {
    for kind in 3 {
        let id = thread_spawn_contained(fail, kind);
        var lim: u8[..200] = "why=";
        let first = thread_wait(id, lim);
        var grow: u8[>..] = [];
        let second = thread_wait(id, grow);
        print(kind, ": ", first, " ", second, " ", lim[..located(lim)], " ", grow.len);
    }
    // A message that does not fit the limited array's room aborts the wait.
    let id = thread_spawn_contained(fail, 1);
    var small: u8[..8] = "12345";
    thread_wait(id, small);
    print("not reached");
}
