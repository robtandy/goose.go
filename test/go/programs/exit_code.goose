// exit(code) ends the program at once with that status, after its output,
// running no defer (§9.3).

fn finish(code: i64) {
    defer print("not printed");
    print("exiting with ", code);
    exit(code);
}

fn main() {
    print("start");
    var n = 0;
    while true {
        n += 1;
        if n == 3 { finish(n + 4); }
    }
}
